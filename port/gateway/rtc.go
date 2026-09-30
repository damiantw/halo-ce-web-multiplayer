package main

// WebRTC for the game's datagrams (docs/gateway.md, "WebRTC").
//
// The WebSocket stays: it carries the join token, HELLO, the TCP streams and
// the signaling. Once the page's data channel is open (unordered, no
// retransmissions: a datagram lost is lost, as over UDP), UDP frames and
// PING go over it, both ways; the WebSocket still accepts them, so a switch
// in either direction loses nothing but what was in flight. Should the
// channel fail or go quiet (no frame for rtcStaleAfter; the page sends a
// keepalive PING every half second), both ends go back to the WebSocket, and the
// game carries on.
//
// Signaling is frame 8, a JSON object, over the WebSocket:
//
//	client -> gateway   {"type":"offer","sdp":...}   the page's offer, with its data channel
//	                    {"type":"candidate","candidate":{...}}   (optional; an ICE-lite gateway does not need them)
//	                    {"type":"bye","reason":...}  the page gave up on the channel
//	gateway -> client   {"type":"answer","sdp":...}
//	                    {"type":"bye","reason":...}  no WebRTC here, or the channel failed
//
// The gateway is an ICE-lite agent on one UDP port (HALO_GATEWAY_RTC_LISTEN,
// all sessions multiplexed by ICE username), and its answer offers host
// candidates on the public addresses (HALO_GATEWAY_RTC_PUBLIC_IPS): the
// browser connects to that port, so only it needs to be open in the
// firewall. No STUN or TURN servers are involved.

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/logging"
	"github.com/pion/webrtc/v4"
)

const (
	frameRTC = 8

	// datagrams wait in the channel's send buffer at most this much: more
	// is a congested path, and a late datagram is worse than a lost one
	rtcMaxBuffered = 256 * 1024
	rtcMaxSignal   = 16 * 1024
)

// A session's offers are limited: each makes a peer connection (DTLS keys,
// an SCTP association), so a page (or a hostile client) repeating them must
// not cost the gateway more than a few. The page makes one per WebSocket
// connection; a session takes rtcMaxOffers in all, one at a time, at least
// rtcOfferInterval apart. Others are refused with a bye ("too many offers",
// "offer too soon"), and the datagrams stay on (or go back to) the WebSocket.
const rtcMaxOffers = 5

var rtcOfferInterval = 2 * time.Second // a variable for the tests

// the page sends a keepalive every half second; a channel this long silent
// is given up (the page gives up after 2.5 s; a variable for the tests)
var rtcStaleAfter = 3 * time.Second

type rtcConfig struct {
	Listen    string       // UDP address to bind (":3478"); empty: no WebRTC
	PublicIPs []netip.Addr // addresses announced as host candidates (empty: the interfaces')
	Port      uint16       // the port announced (0: the bound one); differs behind a port mapping
}

type rtcServer struct {
	api  *webrtc.API
	conn net.PacketConn
	mux  ice.UDPMux
	cfg  rtcConfig
	port uint16
}

func newRTCServer(cfg rtcConfig) (*rtcServer, error) {
	conn, err := net.ListenPacket("udp", cfg.Listen)
	if err != nil {
		return nil, fmt.Errorf("rtc %s: %w", cfg.Listen, err)
	}
	port := uint16(conn.LocalAddr().(*net.UDPAddr).Port)
	if cfg.Port == 0 {
		cfg.Port = port
	}
	factory := logging.NewDefaultLoggerFactory()
	factory.DefaultLogLevel = logging.LogLevelError
	mux := webrtc.NewICEUDPMux(factory.NewLogger("ice"), conn)
	var se webrtc.SettingEngine
	se.LoggerFactory = factory
	se.SetLite(true)
	se.SetICEUDPMux(mux)
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4, webrtc.NetworkTypeUDP6})
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)
	se.SetIncludeLoopbackCandidate(true)
	se.SetICETimeouts(4*time.Second, 8*time.Second, time.Second)
	return &rtcServer{api: webrtc.NewAPI(webrtc.WithSettingEngine(se)), conn: conn, mux: mux, cfg: cfg, port: port}, nil
}

func (r *rtcServer) close() {
	if r != nil {
		r.mux.Close()
		r.conn.Close()
	}
}

// rewriteCandidates puts the public addresses in the answer: one host
// candidate each, on the announced port, in place of what pion found on the
// interfaces (inside a container, addresses nobody can reach).
func (r *rtcServer) rewriteCandidates(sdp string) string {
	if len(r.cfg.PublicIPs) == 0 && r.cfg.Port == r.port {
		return sdp
	}
	lines := strings.Split(strings.ReplaceAll(sdp, "\r\n", "\n"), "\n")
	out := make([]string, 0, len(lines)+len(r.cfg.PublicIPs))
	var found []string
	for _, line := range lines {
		if strings.HasPrefix(line, "a=candidate:") {
			found = append(found, line)
			continue
		}
		if strings.HasPrefix(line, "a=end-of-candidates") {
			continue
		}
		out = append(out, line)
	}
	var candidates []string
	if len(r.cfg.PublicIPs) > 0 {
		for i, ip := range r.cfg.PublicIPs {
			// priority: type preference 126 (host), local preference by order, component 1
			priority := 126<<24 | (65535-i)<<8 | 255
			candidates = append(candidates, fmt.Sprintf("a=candidate:%d 1 udp %d %s %d typ host", i+1, priority, ip.String(), r.cfg.Port))
		}
	} else {
		for _, c := range found {
			f := strings.Fields(c)
			if len(f) > 5 {
				f[5] = strconv.Itoa(int(r.cfg.Port))
			}
			candidates = append(candidates, strings.Join(f, " "))
		}
	}
	// after the media section's attributes (there is one: the data channel)
	for i := len(out) - 1; i >= 0; i-- {
		if out[i] != "" {
			rest := append([]string{}, out[i+1:]...)
			out = append(append(append(out[:i+1], candidates...), "a=end-of-candidates"), rest...)
			break
		}
	}
	return strings.Join(out, "\r\n")
}

type rtcSignal struct {
	Type      string          `json:"type"`
	SDP       string          `json:"sdp,omitempty"`
	Candidate json.RawMessage `json:"candidate,omitempty"`
	Reason    string          `json:"reason,omitempty"`
}

func encodeRTC(sig rtcSignal) []byte {
	body, _ := json.Marshal(sig)
	return append([]byte{frameRTC}, body...)
}

// rtcPeer is a session's peer connection and, once open, its data channel.
type rtcPeer struct {
	s                            *session
	pc                           *webrtc.PeerConnection
	dc                           atomic.Pointer[webrtc.DataChannel]
	lastRx                       atomic.Int64 // unix nanoseconds of the last frame from the channel
	done                         atomic.Bool
	stop                         chan struct{}
	stopper                      sync.Once
	opened                       time.Time
	framesIn, framesOut, dropped atomic.Int64
}

// active is the open data channel, or nil (the WebSocket carries the datagrams)
func (p *rtcPeer) active() *webrtc.DataChannel {
	if p == nil || p.done.Load() {
		return nil
	}
	return p.dc.Load()
}

// send puts a frame on the channel; false if it cannot (the caller uses the
// WebSocket)
func (p *rtcPeer) send(f []byte) bool {
	dc := p.active()
	if dc == nil {
		return false
	}
	if dc.BufferedAmount() > rtcMaxBuffered {
		// congested: drop, as a router would
		p.dropped.Add(1)
		p.s.dropped.Add(1)
		p.s.g.stats.dropped.Add(1)
		return true
	}
	if err := dc.Send(f); err != nil {
		p.fail("send: " + err.Error())
		return false
	}
	p.framesOut.Add(1)
	return true
}

// fail gives the channel up: the datagrams go back to the WebSocket, and
// the page is told
func (p *rtcPeer) fail(reason string) {
	p.shutdown(reason, true)
}

func (p *rtcPeer) shutdown(reason string, tell bool) {
	if !p.done.CompareAndSwap(false, true) {
		return
	}
	p.stopper.Do(func() { close(p.stop) })
	s := p.s
	s.mu.Lock()
	if s.rtc == p {
		s.rtc = nil
	}
	s.mu.Unlock()
	wasOpen := p.dc.Load() != nil
	go p.pc.Close()
	// a fallback: the session goes on over the WebSocket
	if wasOpen && s.ctx.Err() == nil && reason != "session closed" && reason != "replaced by a new offer" {
		s.g.stats.rtcFallbacks.Add(1)
	}
	s.g.log.Info("rtc closed", "sub", s.claims.Sub, "sid", s.claims.Sid, "addr", s.addr.String(), "reason", reason,
		"was_open", wasOpen, "frames_in", p.framesIn.Load(), "frames_out", p.framesOut.Load(), "dropped", p.dropped.Load())
	if tell && s.ctx.Err() == nil {
		s.sendSignal(rtcSignal{Type: "bye", Reason: reason})
	}
}

func (p *rtcPeer) watch() {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-p.stop:
			return
		case <-p.s.ctx.Done():
			p.shutdown("session closed", false)
			return
		case <-t.C:
			if p.dc.Load() == nil {
				// not open yet: the page gives up after a few seconds and says bye;
				// do not keep a half-made peer forever
				if time.Since(p.opened) > 20*time.Second {
					p.fail("not connected")
					return
				}
				continue
			}
			if time.Since(time.Unix(0, p.lastRx.Load())) > rtcStaleAfter {
				p.fail("no traffic")
				return
			}
		}
	}
}

// sendSignal queues a signaling frame on the WebSocket (never on the channel)
func (s *session) sendSignal(sig rtcSignal) {
	select {
	case s.out <- encodeRTC(sig):
	case <-s.ctx.Done():
	case <-time.After(2 * time.Second):
	}
}

// transport is what carries the session's datagrams now: "rtc" or "ws"
func (s *session) transport() string {
	if s.currentRTC().active() != nil {
		return "rtc"
	}
	return "ws"
}

func (s *session) currentRTC() *rtcPeer {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.rtc
}

// handleSignal is frame 8 from the WebSocket.
func (s *session) handleSignal(f []byte) {
	if len(f) > rtcMaxSignal {
		return
	}
	var sig rtcSignal
	if err := json.Unmarshal(f[1:], &sig); err != nil {
		return
	}
	switch sig.Type {
	case "offer":
		if s.g.rtc == nil {
			s.sendSignal(rtcSignal{Type: "bye", Reason: "unavailable"})
			return
		}
		if reason := s.takeOffer(); reason != "" {
			s.g.stats.rtcOffersRefused.Add(1)
			s.g.log.Warn("rtc offer refused", "sub", s.claims.Sub, "addr", s.addr.String(), "err", reason)
			s.sendSignal(rtcSignal{Type: "bye", Reason: reason})
			return
		}
		// answering waits for the gathering (quick: host candidates on the mux)
		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			defer s.rtcAnswering.Store(false)
			if err := s.answer(sig.SDP); err != nil {
				s.g.log.Warn("rtc offer refused", "sub", s.claims.Sub, "addr", s.addr.String(), "err", err.Error())
				s.sendSignal(rtcSignal{Type: "bye", Reason: "offer refused"})
			}
		}()
	case "candidate":
		if p := s.currentRTC(); p != nil && len(sig.Candidate) > 0 {
			var c webrtc.ICECandidateInit
			if json.Unmarshal(sig.Candidate, &c) == nil && c.Candidate != "" {
				p.pc.AddICECandidate(c)
			}
		}
	case "bye":
		if p := s.currentRTC(); p != nil {
			p.shutdown("client: "+sig.Reason, false)
		}
	}
}

// takeOffer counts an offer against the session's limits: "" if it is
// taken, else the reason it is refused
func (s *session) takeOffer() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now()
	switch {
	case s.rtcOffers >= rtcMaxOffers:
		return "too many offers"
	case s.rtcAnswering.Load() || (!s.rtcLastOffer.IsZero() && now.Sub(s.rtcLastOffer) < rtcOfferInterval):
		return "offer too soon"
	}
	s.rtcOffers++
	s.rtcLastOffer = now
	s.rtcAnswering.Store(true)
	return ""
}

func (s *session) answer(sdp string) error {
	if sdp == "" {
		return errors.New("empty offer")
	}
	pc, err := s.g.rtc.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return err
	}
	p := &rtcPeer{s: s, pc: pc, stop: make(chan struct{}), opened: time.Now()}
	s.mu.Lock()
	old := s.rtc
	s.rtc = p
	s.mu.Unlock()
	if old != nil {
		old.shutdown("replaced by a new offer", false)
	}
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		switch state {
		case webrtc.PeerConnectionStateFailed, webrtc.PeerConnectionStateClosed:
			p.fail("connection " + state.String())
		}
	})
	pc.OnDataChannel(func(dc *webrtc.DataChannel) {
		if dc.Label() != "halo" || dc.Ordered() {
			s.g.log.Warn("rtc channel refused", "addr", s.addr.String(), "label", dc.Label(), "ordered", dc.Ordered())
			dc.Close()
			return
		}
		dc.OnOpen(func() {
			if p.done.Load() {
				return
			}
			p.lastRx.Store(time.Now().UnixNano())
			p.dc.Store(dc)
			s.g.stats.rtcOpened.Add(1)
			s.g.log.Info("rtc open", "sub", s.claims.Sub, "sid", s.claims.Sid, "addr", s.addr.String(),
				"setup_ms", time.Since(p.opened).Milliseconds())
		})
		dc.OnClose(func() { p.fail("channel closed") })
		dc.OnMessage(func(msg webrtc.DataChannelMessage) {
			if p.done.Load() || msg.IsString || len(msg.Data) == 0 {
				return
			}
			p.lastRx.Store(time.Now().UnixNano())
			p.framesIn.Add(1)
			s.receive(msg.Data, p)
		})
	})
	if err := pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: sdp}); err != nil {
		p.shutdown("bad offer", false)
		return err
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		p.shutdown("no answer", false)
		return err
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(answer); err != nil {
		p.shutdown("no answer", false)
		return err
	}
	select {
	case <-gathered:
	case <-time.After(3 * time.Second):
	case <-s.ctx.Done():
		p.shutdown("session closed", false)
		return nil
	}
	local := pc.LocalDescription()
	if local == nil {
		p.shutdown("no answer", false)
		return errors.New("no local description")
	}
	s.wg.Add(1)
	go func() { defer s.wg.Done(); p.watch() }()
	s.sendSignal(rtcSignal{Type: "answer", SDP: s.g.rtc.rewriteCandidates(local.SDP)})
	return nil
}
