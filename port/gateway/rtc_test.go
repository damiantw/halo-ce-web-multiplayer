package main

import (
	"encoding/json"
	"net/netip"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/pion/webrtc/v4"
)

func TestRewriteCandidates(t *testing.T) {
	r := &rtcServer{port: 40000, cfg: rtcConfig{Port: 3478, PublicIPs: []netip.Addr{
		netip.MustParseAddr("64.176.197.12"), netip.MustParseAddr("2001:19f0:5:1c9:5400:6ff:fec4:6c76")}}}
	sdp := "v=0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:0\r\n" +
		"a=candidate:1 1 udp 2130706431 172.18.0.3 40000 typ host\r\na=end-of-candidates\r\na=sctp-port:5000\r\n"
	out := r.rewriteCandidates(sdp)
	if strings.Contains(out, "172.18.0.3") {
		t.Fatalf("private candidate kept:\n%s", out)
	}
	for _, want := range []string{"64.176.197.12 3478 typ host", "2001:19f0:5:1c9:5400:6ff:fec4:6c76 3478 typ host", "a=sctp-port:5000", "a=end-of-candidates"} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	if strings.Count(out, "a=end-of-candidates") != 1 {
		t.Errorf("end-of-candidates repeated:\n%s", out)
	}
	// no public addresses: the interfaces' candidates, with the announced port
	r = &rtcServer{port: 40000, cfg: rtcConfig{Port: 3478}}
	if out := r.rewriteCandidates(sdp); !strings.Contains(out, "172.18.0.3 3478 typ host") {
		t.Errorf("port not announced:\n%s", out)
	}
}

func rtcFixture(t *testing.T) *fixture {
	return newFixture(t, func(c *config) {
		c.RTC = rtcConfig{Listen: "127.0.0.1:0", PublicIPs: []netip.Addr{netip.MustParseAddr("127.0.0.1")}}
	})
}

// rtcClient is a browser stand-in: a pion peer whose data channel is made
// as the page makes it (unordered, no retransmissions).
type rtcClient struct {
	t    *testing.T
	pc   *webrtc.PeerConnection
	dc   *webrtc.DataChannel
	open chan struct{}
	in   chan []byte
}

func newRTCClient(t *testing.T) *rtcClient {
	t.Helper()
	var se webrtc.SettingEngine
	se.SetIncludeLoopbackCandidate(true)
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	pc, err := webrtc.NewAPI(webrtc.WithSettingEngine(se)).NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { pc.Close() })
	ordered, retransmits := false, uint16(0)
	dc, err := pc.CreateDataChannel("halo", &webrtc.DataChannelInit{Ordered: &ordered, MaxRetransmits: &retransmits})
	if err != nil {
		t.Fatal(err)
	}
	c := &rtcClient{t: t, pc: pc, dc: dc, open: make(chan struct{}), in: make(chan []byte, 64)}
	dc.OnOpen(func() { close(c.open) })
	dc.OnMessage(func(m webrtc.DataChannelMessage) { c.in <- append([]byte(nil), m.Data...) })
	return c
}

func sendSig(t *testing.T, ws *websocket.Conn, sig rtcSignal) {
	t.Helper()
	write(t, ws, encodeRTC(sig))
}

// readSignal reads WebSocket frames until a signaling one
func readSignal(t *testing.T, ws *websocket.Conn) rtcSignal {
	t.Helper()
	for {
		f := read(t, ws)
		if f[0] != frameRTC {
			continue
		}
		var sig rtcSignal
		if err := json.Unmarshal(f[1:], &sig); err != nil {
			t.Fatal(err)
		}
		return sig
	}
}

// connect does the page's offer and waits for the channel
func (c *rtcClient) connect(ws *websocket.Conn) string {
	c.t.Helper()
	offer, err := c.pc.CreateOffer(nil)
	if err != nil {
		c.t.Fatal(err)
	}
	if err := c.pc.SetLocalDescription(offer); err != nil {
		c.t.Fatal(err)
	}
	sendSig(c.t, ws, rtcSignal{Type: "offer", SDP: offer.SDP})
	answer := readSignal(c.t, ws)
	if answer.Type != "answer" {
		c.t.Fatalf("want an answer, got %+v", answer)
	}
	if err := c.pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeAnswer, SDP: answer.SDP}); err != nil {
		c.t.Fatal(err)
	}
	select {
	case <-c.open:
	case <-time.After(5 * time.Second):
		c.t.Fatal("data channel did not open")
	}
	return answer.SDP
}

func (c *rtcClient) recv() []byte {
	c.t.Helper()
	select {
	case f := <-c.in:
		return f
	case <-time.After(3 * time.Second):
		c.t.Fatal("nothing on the data channel")
		return nil
	}
}

func waitTransport(t *testing.T, g *gateway, want string) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for {
		got := "none"
		if l := g.sessionList(); len(l) == 1 {
			got = l[0].transport()
		}
		if got == want {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("transport %s, want %s", got, want)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestRTCDatagrams(t *testing.T) {
	f := rtcFixture(t)
	server := listenUDP(t, f.server, 5150)
	ws, me := f.join(claims{Sub: "1"})
	c := newRTCClient(t)
	sdp := c.connect(ws)
	if !strings.Contains(sdp, "127.0.0.1 ") || !strings.Contains(sdp, "typ host") {
		t.Fatalf("answer lacks the announced candidate:\n%s", sdp)
	}
	waitTransport(t, f.g, "rtc")

	// client -> server over the channel
	if err := c.dc.Send(encodeClientUDP(f.server, 5150, 5150, []byte("up"))); err != nil {
		t.Fatal(err)
	}
	got, from := recvUDP(t, server)
	if string(got) != "up" || from != netip.AddrPortFrom(me, 5150) {
		t.Fatalf("server got %q from %v", got, from)
	}
	// server -> client over the channel, not the WebSocket
	server.WriteToUDPAddrPort([]byte("down"), from)
	u, err := parseUDP(c.recv())
	if err != nil || string(u.Payload) != "down" || u.Addr != f.server || u.Port != 5150 || u.Local != 5150 {
		t.Fatalf("client got %+v %v", u, err)
	}
	// a PING over the channel is answered there
	c.dc.Send([]byte{framePing, 'k'})
	if p := c.recv(); len(p) != 2 || p[0] != framePing {
		t.Fatalf("ping answer %v", p)
	}
	// the WebSocket still takes datagrams (a page between the two)
	write(t, ws, encodeClientUDP(f.server, 5150, 5150, []byte("ws")))
	if got, _ := recvUDP(t, server); string(got) != "ws" {
		t.Fatalf("server got %q", got)
	}
	// streams are not accepted on the channel: no OPENED comes back anywhere
	c.dc.Send(encodeOpen(1, f.server, 5150, 49152))
	select {
	case fr := <-c.in:
		t.Fatalf("channel answered a stream frame: %v", fr)
	case <-time.After(300 * time.Millisecond):
	}
}

func TestRTCFallbackWhenChannelCloses(t *testing.T) {
	f := rtcFixture(t)
	server := listenUDP(t, f.server, 5150)
	ws, me := f.join(claims{Sub: "1"})
	c := newRTCClient(t)
	c.connect(ws)
	waitTransport(t, f.g, "rtc")
	c.pc.Close()
	waitTransport(t, f.g, "ws")
	// the game goes on over the WebSocket
	server.WriteToUDPAddrPort([]byte("after"), netip.AddrPortFrom(me, 5150))
	for {
		fr := read(t, ws)
		if fr[0] == frameUDP {
			if u, _ := parseUDP(fr); string(u.Payload) != "after" {
				t.Fatalf("got %q", u.Payload)
			}
			break
		}
	}
	if n := f.g.stats.rtcFallbacks.Load(); n != 1 {
		t.Fatalf("fallbacks %d", n)
	}
}

func TestRTCFallbackWhenChannelGoesQuiet(t *testing.T) {
	old := rtcStaleAfter
	rtcStaleAfter = 1500 * time.Millisecond
	t.Cleanup(func() { rtcStaleAfter = old })
	f := rtcFixture(t)
	ws, _ := f.join(claims{Sub: "1"})
	c := newRTCClient(t)
	c.connect(ws)
	waitTransport(t, f.g, "rtc")
	// the page sends nothing (its UDP blocked): the gateway gives up and says so
	start := time.Now()
	bye := readSignal(t, ws)
	if bye.Type != "bye" || bye.Reason != "no traffic" {
		t.Fatalf("want bye (no traffic), got %+v", bye)
	}
	if d := time.Since(start); d > 4*time.Second {
		t.Fatalf("bye after %v", d)
	}
	waitTransport(t, f.g, "ws")
}

func TestRTCClientBye(t *testing.T) {
	f := rtcFixture(t)
	ws, _ := f.join(claims{Sub: "1"})
	c := newRTCClient(t)
	c.connect(ws)
	waitTransport(t, f.g, "rtc")
	sendSig(t, ws, rtcSignal{Type: "bye", Reason: "test"})
	waitTransport(t, f.g, "ws")
}

func TestRTCUnavailable(t *testing.T) {
	f := newFixture(t, nil)
	ws, _ := f.join(claims{Sub: "1"})
	c := newRTCClient(t)
	offer, _ := c.pc.CreateOffer(nil)
	c.pc.SetLocalDescription(offer)
	sendSig(t, ws, rtcSignal{Type: "offer", SDP: offer.SDP})
	if bye := readSignal(t, ws); bye.Type != "bye" || bye.Reason != "unavailable" {
		t.Fatalf("want bye (unavailable), got %+v", bye)
	}
}

func TestRTCBadOffer(t *testing.T) {
	f := rtcFixture(t)
	ws, _ := f.join(claims{Sub: "1"})
	sendSig(t, ws, rtcSignal{Type: "offer", SDP: "v=0 nonsense"})
	if bye := readSignal(t, ws); bye.Type != "bye" {
		t.Fatalf("want bye, got %+v", bye)
	}
	// the session is unharmed
	write(t, ws, []byte{framePing, 'x'})
	for {
		if fr := read(t, ws); fr[0] == framePing {
			break
		}
	}
}
