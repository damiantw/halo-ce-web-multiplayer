package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

// config is the gateway's settings (main.go reads them from the
// environment).
type config struct {
	Secret        []byte
	ClientNet     netip.Prefix // the browsers' addresses (127.64.0.0/16)
	Hub           netip.Addr   // where servers send their broadcasts (network.broadcast)
	Ports         []uint16     // the server ports a client may reach (5150, 5151)
	MaxClients    int
	MaxFrame      int     // bytes in one WebSocket message
	FrameRate     float64 // frames a second from one client (0: no limit)
	FrameBurst    float64
	ByteRate      float64 // bytes a second from one client (0: no limit)
	ByteBurst     float64
	MaxStreams    int
	MaxUDPPorts   int
	DialTimeout   time.Duration
	IdleTimeout   time.Duration // no frame from the client for this long closes it
	PrebindPorts  []uint16      // the client ports bound when a session starts
	AllowInsecure bool          // no secret: accept unsigned test tokens (development)
}

func defaultConfig() config {
	return config{
		ClientNet:    netip.MustParsePrefix("127.64.0.0/16"),
		Hub:          netip.MustParseAddr("127.64.0.1"),
		Ports:        []uint16{5150, 5151},
		MaxClients:   256,
		MaxFrame:     64 * 1024,
		FrameRate:    600,
		FrameBurst:   1200,
		ByteRate:     2 << 20,
		ByteBurst:    4 << 20,
		MaxStreams:   16,
		MaxUDPPorts:  8,
		DialTimeout:  5 * time.Second,
		IdleTimeout:  60 * time.Second,
		PrebindPorts: []uint16{5150, 5151},
	}
}

type gateway struct {
	cfg      config
	log      *slog.Logger
	reg      *registry
	verify   *verifier
	mu       sync.Mutex
	sessions map[netip.Addr]*session
	nextAddr uint32
	hubs     []*net.UDPConn
	wg       sync.WaitGroup
	closing  atomic.Bool
	stats    struct {
		accepted, rejected, framesIn, framesOut, dropped atomic.Int64
	}
}

func newGateway(cfg config, log *slog.Logger, reg *registry) *gateway {
	return &gateway{cfg: cfg, log: log, reg: reg, verify: newVerifier(cfg.Secret), sessions: map[netip.Addr]*session{}}
}

// start binds the hub's sockets: the servers' broadcasts (game
// advertisements) arrive there and go to every client allowed to see the
// server.
func (g *gateway) start() error {
	for _, port := range g.cfg.Ports {
		conn, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(g.cfg.Hub, port)))
		if err != nil {
			g.stopHubs()
			return fmt.Errorf("hub %s:%d: %w", g.cfg.Hub, port, err)
		}
		g.hubs = append(g.hubs, conn)
		g.wg.Add(1)
		go g.hubLoop(conn, port)
	}
	return nil
}

func (g *gateway) stopHubs() {
	for _, h := range g.hubs {
		h.Close()
	}
}

func (g *gateway) hubLoop(conn *net.UDPConn, port uint16) {
	defer g.wg.Done()
	buf := make([]byte, 65536)
	for {
		n, from, err := conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			return
		}
		src := from.Addr().Unmap()
		id, ok := g.reg.idOf(src)
		if !ok {
			continue
		}
		frame := encodeUDP(src, from.Port(), port, buf[:n])
		for _, s := range g.sessionList() {
			if permits(s.claims.Srv, id) {
				s.sendDroppable(frame)
			}
		}
	}
}

func (g *gateway) sessionList() []*session {
	g.mu.Lock()
	defer g.mu.Unlock()
	out := make([]*session, 0, len(g.sessions))
	for _, s := range g.sessions {
		if s != nil {
			out = append(out, s)
		}
	}
	return out
}

// allocate claims a client address: the token's, or the next free one.
func (g *gateway) allocate(requested string) (netip.Addr, error) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.closing.Load() {
		return netip.Addr{}, errors.New("shutting down")
	}
	if len(g.sessions) >= g.cfg.MaxClients {
		return netip.Addr{}, errors.New("too many clients")
	}
	usable := func(a netip.Addr) bool {
		if !a.Is4() || !g.cfg.ClientNet.Contains(a) || a == g.cfg.Hub || a == g.cfg.ClientNet.Addr() {
			return false
		}
		b := a.As4()
		return b[3] != 0 && b[3] != 255
	}
	if requested != "" {
		a, err := netip.ParseAddr(requested)
		if err != nil || !usable(a) {
			return netip.Addr{}, fmt.Errorf("address %q is not a client address", requested)
		}
		if holder, taken := g.sessions[a]; taken {
			return netip.Addr{}, &addressInUse{addr: a, holder: holder}
		}
		g.sessions[a] = nil
		return a, nil
	}
	base := g.cfg.ClientNet.Masked().Addr().As4()
	size := uint32(1) << (32 - g.cfg.ClientNet.Bits())
	start := uint32(base[0])<<24 | uint32(base[1])<<16 | uint32(base[2])<<8 | uint32(base[3])
	for i := uint32(0); i < size; i++ {
		g.nextAddr = (g.nextAddr + 1) % size
		v := start + g.nextAddr
		a := netip.AddrFrom4([4]byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)})
		if _, taken := g.sessions[a]; usable(a) && !taken {
			g.sessions[a] = nil
			return a, nil
		}
	}
	return netip.Addr{}, errors.New("no free client address")
}

// addressInUse: the token's address belongs to a live session (holder; nil
// while it is still being set up).
type addressInUse struct {
	addr   netip.Addr
	holder *session
}

func (e *addressInUse) Error() string { return fmt.Sprintf("address %s in use", e.addr) }

// replaceWait is how long a newer session of the same user waits for the
// one it replaces to let go of the address.
const replaceWait = 8 * time.Second

// takeOver closes the session holding a's address for the same user (a
// reload, or a second tab: the site pins one address per user) and claims
// the address once it is free. The old session is closed with 1008, so its
// page, should it wake up, gives up rather than take the address back.
func (g *gateway) takeOver(err error, c claims, remote string) (netip.Addr, error) {
	var inUse *addressInUse
	if !errors.As(err, &inUse) || inUse.holder == nil || c.Sub == "" || inUse.holder.claims.Sub != c.Sub {
		return netip.Addr{}, err
	}
	g.log.Info("session replaced", "sub", c.Sub, "old_sid", inUse.holder.claims.Sid, "sid", c.Sid,
		"addr", inUse.addr.String(), "remote", remote)
	inUse.holder.close(websocket.StatusPolicyViolation, "replaced by a newer session")
	deadline := time.Now().Add(replaceWait)
	for {
		a, err := g.allocate(c.Adr)
		if !errors.As(err, &inUse) || time.Now().After(deadline) {
			return a, err
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func (g *gateway) release(a netip.Addr) {
	g.mu.Lock()
	delete(g.sessions, a)
	g.mu.Unlock()
}

// ServeHTTP is the WebSocket endpoint (/gateway behind nginx).
func (g *gateway) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	remote := r.Header.Get("X-Forwarded-For")
	if remote == "" {
		remote = r.RemoteAddr
	}
	token := tokenFromProtocols(r.Header.Values("Sec-WebSocket-Protocol"))
	c, err := g.verify.verify(token)
	if err != nil {
		g.stats.rejected.Add(1)
		g.log.Warn("join refused", "reason", err.Error(), "remote", remote)
		http.Error(w, "join token refused: "+err.Error(), http.StatusUnauthorized)
		return
	}
	addr, err := g.allocate(c.Adr)
	if err != nil {
		addr, err = g.takeOver(err, c, remote)
	}
	if err != nil {
		g.stats.rejected.Add(1)
		g.log.Warn("join refused", "reason", err.Error(), "sub", c.Sub, "sid", c.Sid, "remote", remote)
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	ws, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		Subprotocols:       []string{"halo.v1"},
		InsecureSkipVerify: true, // the Origin is checked by nginx/Laravel; tokens authenticate
	})
	if err != nil {
		g.release(addr)
		g.log.Warn("websocket accept failed", "err", err.Error(), "remote", remote)
		return
	}
	ws.SetReadLimit(int64(g.cfg.MaxFrame))
	s := newSession(g, ws, addr, c)
	g.mu.Lock()
	g.sessions[addr] = s
	g.mu.Unlock()
	g.stats.accepted.Add(1)
	g.log.Info("session opened", "sub", c.Sub, "sid", c.Sid, "addr", addr.String(), "servers", c.Srv, "remote", remote)
	g.wg.Add(1)
	defer g.wg.Done()
	reason := s.run(r.Context())
	g.release(addr)
	g.log.Info("session closed", "sub", c.Sub, "sid", c.Sid, "addr", addr.String(), "reason", reason,
		"frames_in", s.framesIn.Load(), "frames_out", s.framesOut.Load(), "dropped", s.dropped.Load())
}

// shutdown closes every session (close code 1001) and the hub, and waits
// for the goroutines, at most until ctx ends.
func (g *gateway) shutdown(ctx context.Context) {
	g.closing.Store(true)
	for _, s := range g.sessionList() {
		if s != nil {
			s.close(websocket.StatusGoingAway, "gateway shutting down")
		}
	}
	g.stopHubs()
	done := make(chan struct{})
	go func() { g.wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-ctx.Done():
		g.log.Warn("shutdown timed out")
	}
}
