package main

import (
	"context"
	"errors"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

// session is one browser: its WebSocket, its loopback address, the UDP
// sockets bound there for the client's ports, and its TCP streams.
type session struct {
	g      *gateway
	ws     *websocket.Conn
	addr   netip.Addr
	claims claims

	out     chan []byte // frames to the client
	ctx     context.Context
	cancel  context.CancelFunc
	closeMu sync.Mutex
	closed  bool
	code    websocket.StatusCode
	why     string
	frames  *bucket
	bytes   *bucket
	mu      sync.Mutex
	udp     map[uint16]*net.UDPConn
	streams map[uint32]*net.TCPConn
	rtc     *rtcPeer // the WebRTC peer (rtc.go), nil without one; under mu
	// the offers taken (rtc.go, takeOffer): how many, the last one's time
	// (under mu), and whether one is being answered
	rtcOffers    int
	rtcLastOffer time.Time
	rtcAnswering atomic.Bool
	framesIn     atomic.Int64
	framesOut    atomic.Int64
	dropped      atomic.Int64
	lastIn       atomic.Int64 // unix nanoseconds of the last frame from the client, either transport
	wg           sync.WaitGroup
}

func newSession(g *gateway, ws *websocket.Conn, addr netip.Addr, c claims) *session {
	ctx, cancel := context.WithCancel(context.Background())
	return &session{
		g: g, ws: ws, addr: addr, claims: c,
		out: make(chan []byte, 2048), ctx: ctx, cancel: cancel,
		frames:  newBucket(g.cfg.FrameRate, g.cfg.FrameBurst),
		bytes:   newBucket(g.cfg.ByteRate, g.cfg.ByteBurst),
		udp:     map[uint16]*net.UDPConn{},
		streams: map[uint32]*net.TCPConn{},
	}
}

func (s *session) close(code websocket.StatusCode, why string) {
	s.closeMu.Lock()
	first := !s.closed
	if first {
		s.closed, s.code, s.why = true, code, why
	}
	s.closeMu.Unlock()
	s.cancel()
	if first && code != websocket.StatusAbnormalClosure {
		// the close handshake; the reader sees the answer and stops
		go s.ws.Close(code, why)
	}
}

// sendDroppable sends a datagram's frame: on the data channel when there
// is one (rtc.go), else queued for the WebSocket; a full queue drops it, as
// the network would.
func (s *session) sendDroppable(f []byte) {
	if p := s.currentRTC(); p != nil && p.send(f) {
		s.framesOut.Add(1)
		s.g.stats.framesOut.Add(1)
		s.g.stats.bytesOut.Add(int64(len(f)))
		return
	}
	s.sendWSDroppable(f)
}

// sendWSDroppable queues a datagram's frame for the WebSocket.
func (s *session) sendWSDroppable(f []byte) {
	select {
	case s.out <- f:
	default:
		s.dropped.Add(1)
		s.g.stats.dropped.Add(1)
	}
}

// sendReliable queues a stream frame, waiting for room.
func (s *session) sendReliable(f []byte) bool {
	select {
	case s.out <- f:
		return true
	case <-s.ctx.Done():
		return false
	}
}

func (s *session) run(parent context.Context) string {
	defer s.cleanup()
	go func() {
		select {
		case <-parent.Done():
			s.close(websocket.StatusGoingAway, "request ended")
		case <-s.ctx.Done():
		}
	}()
	for _, port := range s.g.cfg.PrebindPorts {
		if _, err := s.udpSocket(port); err != nil {
			s.g.log.Warn("prebind failed", "addr", s.addr.String(), "port", port, "err", err.Error())
		}
	}
	s.wg.Add(1)
	go s.writer()
	s.sendReliable(encodeHello(s.addr))
	// Idle is judged on frames from either transport: with a data channel
	// open, a playing client can send nothing on the WebSocket for minutes.
	// (A read deadline would not do: an expired read context closes the
	// connection.) The read itself runs on a context of its own so that the
	// close handshake can finish after s.ctx is cancelled.
	s.lastIn.Store(time.Now().UnixNano())
	go s.idleWatch()
read:
	for {
		typ, frame, err := s.ws.Read(context.Background())
		if err != nil {
			switch {
			case s.ctx.Err() != nil:
				// closed from this side (shutdown, idle, a write failure)
			case websocket.CloseStatus(err) != -1:
				s.close(websocket.CloseStatus(err), "client closed")
			case errors.Is(err, websocket.ErrMessageTooBig) || isTooBig(err):
				s.close(websocket.StatusMessageTooBig, "frame too big")
			default:
				s.close(websocket.StatusAbnormalClosure, "read: "+err.Error())
			}
			break read
		}
		s.lastIn.Store(time.Now().UnixNano())
		if typ != websocket.MessageBinary || len(frame) == 0 {
			continue
		}
		s.receive(frame, nil)
	}
	s.closeMu.Lock()
	code, why := s.code, s.why
	s.closeMu.Unlock()
	return why + " (" + code.String() + ")"
}

// idleWatch closes the session when the client has sent nothing, on the
// WebSocket or the data channel, for IdleTimeout.
func (s *session) idleWatch() {
	tick := time.NewTicker(min(s.g.cfg.IdleTimeout/4, time.Second))
	defer tick.Stop()
	for {
		select {
		case <-s.ctx.Done():
			return
		case <-tick.C:
			if time.Since(time.Unix(0, s.lastIn.Load())) > s.g.cfg.IdleTimeout {
				s.close(websocket.StatusPolicyViolation, "idle")
				return
			}
		}
	}
}

// receive takes a frame from the WebSocket (via nil) or the data channel:
// counted, limited by the session's buckets, then handled. The data channel
// carries only datagrams and PING: streams need the WebSocket's order and
// delivery.
func (s *session) receive(frame []byte, via *rtcPeer) {
	s.lastIn.Store(time.Now().UnixNano())
	if via != nil && frame[0] != frameUDP && frame[0] != framePing {
		return
	}
	s.framesIn.Add(1)
	s.g.stats.framesIn.Add(1)
	s.g.stats.bytesIn.Add(int64(len(frame)))
	if !s.frames.take(1) || !s.bytes.take(float64(len(frame))) {
		s.dropped.Add(1)
		s.g.stats.dropped.Add(1)
		return
	}
	s.handle(frame, via)
}

func isTooBig(err error) bool {
	return err != nil && (containsFold(err.Error(), "read limited") || containsFold(err.Error(), "too big"))
}

func containsFold(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

func (s *session) writer() {
	defer s.wg.Done()
	for {
		select {
		case f := <-s.out:
			ctx, cancel := context.WithTimeout(s.ctx, 10*time.Second)
			err := s.ws.Write(ctx, websocket.MessageBinary, f)
			cancel()
			if err != nil {
				s.close(websocket.StatusAbnormalClosure, "write: "+err.Error())
				return
			}
			s.framesOut.Add(1)
			s.g.stats.framesOut.Add(1)
			s.g.stats.bytesOut.Add(int64(len(f)))
		case <-s.ctx.Done():
			return
		}
	}
}

func (s *session) cleanup() {
	s.cancel()
	s.closeMu.Lock()
	code, why := s.code, s.why
	s.closeMu.Unlock()
	if code == 0 {
		code = websocket.StatusNormalClosure
	}
	if p := s.currentRTC(); p != nil {
		p.shutdown("session closed", false)
	}
	s.mu.Lock()
	for _, c := range s.udp {
		c.Close()
	}
	for _, c := range s.streams {
		c.Close()
	}
	s.mu.Unlock()
	s.wg.Wait()
	if code == websocket.StatusAbnormalClosure {
		s.ws.CloseNow()
	} else {
		s.ws.Close(code, why)
	}
}

func (s *session) portAllowed(p uint16) bool {
	for _, a := range s.g.cfg.Ports {
		if a == p {
			return true
		}
	}
	return false
}

func (s *session) handle(f []byte, via *rtcPeer) {
	switch f[0] {
	case frameUDP:
		u, err := parseUDP(f)
		if err != nil || u.Local == 0 {
			return
		}
		s.handleUDP(u)
	case frameOpen:
		o, err := parseOpen(f)
		if err != nil {
			return
		}
		s.handleOpen(o)
	case frameData:
		id, payload, err := parseStream(f)
		if err != nil {
			return
		}
		s.mu.Lock()
		c := s.streams[id]
		s.mu.Unlock()
		if c == nil {
			return
		}
		c.SetWriteDeadline(time.Now().Add(10 * time.Second))
		if _, err := c.Write(payload); err != nil {
			s.closeStream(id, true)
		}
	case frameClose:
		id, _, err := parseStream(f)
		if err == nil {
			s.closeStream(id, false)
		}
	case framePing:
		if len(f) <= 1+maxPingToken {
			// droppable: a lost answer is a lost ping, as over the network;
			// answered the way it came, so each path's round trip is measured
			echo := append([]byte(nil), f...)
			if via == nil || !via.send(echo) {
				s.sendWSDroppable(echo)
			}
		}
	case frameRTC:
		if via == nil {
			s.handleSignal(f)
		}
	}
}

// targets resolves a client's datagram destination: one allowed server, or
// for a broadcast every allowed server.
func (s *session) targets(dst netip.Addr) []netip.Addr {
	if dst == netip.AddrFrom4([4]byte{255, 255, 255, 255}) || dst == s.g.cfg.Hub {
		return s.g.reg.allowed(s.claims.Srv)
	}
	id, ok := s.g.reg.idOf(dst)
	if !ok || !permits(s.claims.Srv, id) {
		return nil
	}
	return []netip.Addr{dst}
}

func (s *session) handleUDP(u udpFrame) {
	if !s.portAllowed(u.Port) {
		return
	}
	targets := s.targets(u.Addr)
	if len(targets) == 0 {
		return
	}
	conn, err := s.udpSocket(u.Local)
	if err != nil {
		return
	}
	for _, t := range targets {
		conn.WriteToUDPAddrPort(u.Payload, netip.AddrPortFrom(t, u.Port))
	}
}

// udpSocket is the real socket on the client's address and port, bound on
// first use; what arrives there goes to the client.
func (s *session) udpSocket(port uint16) (*net.UDPConn, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if c, ok := s.udp[port]; ok {
		return c, nil
	}
	if len(s.udp) >= s.g.cfg.MaxUDPPorts {
		return nil, errors.New("too many UDP ports")
	}
	c, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(s.addr, port)))
	if err != nil {
		return nil, err
	}
	s.udp[port] = c
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		buf := make([]byte, 65536)
		for {
			n, from, err := c.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			src := from.Addr().Unmap()
			id, ok := s.g.reg.idOf(src)
			if !ok || !permits(s.claims.Srv, id) {
				continue
			}
			s.sendDroppable(encodeUDP(src, from.Port(), port, buf[:n]))
		}
	}()
	return c, nil
}

func (s *session) handleOpen(o openFrame) {
	fail := func() { s.sendReliable(encodeOpened(o.Stream, false)) }
	if len(s.targets(o.Addr)) != 1 || o.Addr == s.g.cfg.Hub || !s.portAllowed(o.Port) {
		fail()
		return
	}
	s.mu.Lock()
	_, dup := s.streams[o.Stream]
	full := len(s.streams) >= s.g.cfg.MaxStreams
	if !dup && !full {
		s.streams[o.Stream] = nil // reserved while dialing
	}
	s.mu.Unlock()
	if dup || full {
		fail()
		return
	}
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		d := net.Dialer{Timeout: s.g.cfg.DialTimeout, LocalAddr: &net.TCPAddr{IP: s.addr.AsSlice()}}
		raw, err := d.DialContext(s.ctx, "tcp4", netip.AddrPortFrom(o.Addr, o.Port).String())
		if err != nil {
			s.mu.Lock()
			delete(s.streams, o.Stream)
			s.mu.Unlock()
			s.g.log.Debug("stream refused", "addr", s.addr.String(), "dst", o.Addr.String(), "err", err.Error())
			fail()
			return
		}
		c := raw.(*net.TCPConn)
		c.SetNoDelay(true)
		s.mu.Lock()
		if s.ctx.Err() != nil {
			s.mu.Unlock()
			c.Close()
			return
		}
		s.streams[o.Stream] = c
		s.mu.Unlock()
		s.sendReliable(encodeOpened(o.Stream, true))
		buf := make([]byte, 16*1024)
		for {
			n, err := c.Read(buf)
			if n > 0 && !s.sendReliable(encodeData(o.Stream, buf[:n])) {
				return
			}
			if err != nil {
				s.closeStream(o.Stream, true)
				return
			}
		}
	}()
}

func (s *session) closeStream(id uint32, tell bool) {
	s.mu.Lock()
	c, ok := s.streams[id]
	delete(s.streams, id)
	s.mu.Unlock()
	if !ok {
		return
	}
	if c != nil {
		c.Close()
	}
	if tell {
		s.sendReliable(encodeClose(id))
	}
}
