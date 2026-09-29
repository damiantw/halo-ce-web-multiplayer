package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/coder/websocket"
)

var testSecret = []byte("0123456789abcdef-test-secret")

// each test gets its own loopback addresses: 127.<70+n>.x.y
var testNet atomic.Int32

type fixture struct {
	t      *testing.T
	g      *gateway
	srv    *httptest.Server
	server netip.Addr // a registered game server's address
	block  byte
}

func newFixture(t *testing.T, tweak func(*config)) *fixture {
	t.Helper()
	block := byte(70 + testNet.Add(1))
	cfg := defaultConfig()
	cfg.Secret = testSecret
	cfg.ClientNet = netip.MustParsePrefix(fmt.Sprintf("127.%d.0.0/16", block))
	cfg.Hub = netip.AddrFrom4([4]byte{127, block, 255, 1})
	cfg.IdleTimeout = 5 * time.Second
	if tweak != nil {
		tweak(&cfg)
	}
	reg := newRegistry()
	server := netip.AddrFrom4([4]byte{127, 1, block, 1})
	if err := reg.set("s1", server); err != nil {
		t.Fatal(err)
	}
	reg.set("s2", netip.AddrFrom4([4]byte{127, 1, block, 2}))
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	if testing.Verbose() {
		log = slog.New(slog.NewTextHandler(testWriter{t}, &slog.HandlerOptions{Level: slog.LevelDebug}))
	}
	g := newGateway(cfg, log, reg)
	if err := g.start(); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(g)
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		g.shutdown(ctx)
		srv.Close()
	})
	return &fixture{t: t, g: g, srv: srv, server: server, block: block}
}

type testWriter struct{ t *testing.T }

func (w testWriter) Write(p []byte) (int, error) {
	w.t.Log(strings.TrimSpace(string(p)))
	return len(p), nil
}

func (f *fixture) token(c claims) string {
	if c.Sid == "" {
		c.Sid = fmt.Sprintf("sid-%d", time.Now().UnixNano())
	}
	if c.Exp == 0 {
		c.Exp = time.Now().Add(time.Minute).Unix()
	}
	if c.Srv == nil {
		c.Srv = []string{"*"}
	}
	return signToken(testSecret, c)
}

func (f *fixture) dial(token string) (*websocket.Conn, *http.Response, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	url := "ws" + strings.TrimPrefix(f.srv.URL, "http")
	return websocket.Dial(ctx, url, &websocket.DialOptions{Subprotocols: []string{"halo.v1", "t." + token}})
}

// join connects and reads HELLO: the client's address
func (f *fixture) join(c claims) (*websocket.Conn, netip.Addr) {
	f.t.Helper()
	ws, _, err := f.dial(f.token(c))
	if err != nil {
		f.t.Fatal(err)
	}
	f.t.Cleanup(func() { ws.CloseNow() })
	if ws.Subprotocol() != "halo.v1" {
		f.t.Fatalf("subprotocol %q", ws.Subprotocol())
	}
	frame := read(f.t, ws)
	if frame[0] != frameHello || len(frame) != 5 {
		f.t.Fatalf("want HELLO, got %v", frame)
	}
	return ws, addr4(frame[1:5])
}

func read(t *testing.T, ws *websocket.Conn) []byte {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, b, err := ws.Read(ctx)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return b
}

func write(t *testing.T, ws *websocket.Conn, f []byte) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := ws.Write(ctx, websocket.MessageBinary, f); err != nil {
		t.Fatalf("write: %v", err)
	}
}

func listenUDP(t *testing.T, a netip.Addr, port uint16) *net.UDPConn {
	t.Helper()
	c, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(a, port)))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

func recvUDP(t *testing.T, c *net.UDPConn) ([]byte, netip.AddrPort) {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	buf := make([]byte, 2048)
	n, from, err := c.ReadFromUDPAddrPort(buf)
	if err != nil {
		t.Fatalf("udp read: %v", err)
	}
	return buf[:n], from
}

func TestFramesRoundTrip(t *testing.T) {
	a := netip.MustParseAddr("127.0.1.7")
	u, err := parseUDP(encodeClientUDP(a, 5150, 5151, []byte("hi")))
	if err != nil || u.Addr != a || u.Port != 5150 || u.Local != 5151 || string(u.Payload) != "hi" {
		t.Fatalf("udp %+v %v", u, err)
	}
	o, err := parseOpen(encodeOpen(9, a, 5150, 49152))
	if err != nil || o.Stream != 9 || o.Addr != a || o.Port != 5150 || o.SrcPort != 49152 {
		t.Fatalf("open %+v %v", o, err)
	}
	id, p, err := parseStream(encodeData(3, []byte("xyz")))
	if err != nil || id != 3 || string(p) != "xyz" {
		t.Fatalf("data %d %q %v", id, p, err)
	}
	if _, err := parseUDP([]byte{frameUDP, 1, 2}); err == nil {
		t.Fatal("short frame accepted")
	}
	if _, err := parseOpen(encodeOpened(1, true)); err == nil {
		t.Fatal("wrong type accepted")
	}
}

func TestTokens(t *testing.T) {
	v := newVerifier(testSecret)
	now := time.Unix(1_800_000_000, 0)
	v.now = func() time.Time { return now }
	good := claims{Sub: "7", Sid: "a", Exp: now.Unix() + 60, Srv: []string{"s1"}}
	if c, err := v.verify(signToken(testSecret, good)); err != nil || c.Sub != "7" {
		t.Fatalf("good token: %v", err)
	}
	cases := map[string]struct {
		token string
		want  error
	}{
		"replayed":  {signToken(testSecret, good), errTokenReplayed},
		"signature": {signToken([]byte("another secret, 16+ bytes"), claims{Sid: "b", Exp: now.Unix() + 60, Srv: []string{"*"}}), errTokenSignature},
		"expired":   {signToken(testSecret, claims{Sid: "c", Exp: now.Unix() - 1, Srv: []string{"*"}}), errTokenExpired},
		"lifetime":  {signToken(testSecret, claims{Sid: "d", Exp: now.Unix() + 3600, Srv: []string{"*"}}), errTokenLifetime},
		"no srv":    {signToken(testSecret, claims{Sid: "e", Exp: now.Unix() + 60}), errTokenClaims},
		"garbage":   {"not-a-token", errTokenFormat},
		"empty":     {"", errTokenFormat},
	}
	for name, c := range cases {
		if _, err := v.verify(c.token); err != c.want {
			t.Errorf("%s: got %v, want %v", name, err, c.want)
		}
	}
	// a replay is possible again once the first use has expired
	now = now.Add(2 * time.Minute)
	good.Exp = now.Unix() + 60
	if _, err := v.verify(signToken(testSecret, good)); err != nil {
		t.Fatalf("sid after expiry: %v", err)
	}
	if got := tokenFromProtocols([]string{"halo.v1, t.abc.def"}); got != "abc.def" {
		t.Fatalf("protocol token %q", got)
	}
}

func TestParseServers(t *testing.T) {
	m, err := parseServers("a=127.0.1.1, b=127.0.1.2")
	if err != nil || len(m) != 2 || m["b"] != netip.MustParseAddr("127.0.1.2") {
		t.Fatalf("%v %v", m, err)
	}
	for _, bad := range []string{"a", "a=::1", "=127.0.0.1", "a=nope"} {
		if _, err := parseServers(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
	r := newRegistry()
	r.set("a", netip.MustParseAddr("127.0.1.1"))
	if err := r.set("b", netip.MustParseAddr("127.0.1.1")); err == nil {
		t.Fatal("duplicate address accepted")
	}
	if got := r.allowed([]string{"a", "zzz"}); len(got) != 1 {
		t.Fatalf("allowed %v", got)
	}
}

func TestBucket(t *testing.T) {
	now := time.Unix(0, 0)
	b := newBucket(10, 2)
	b.now = func() time.Time { return now }
	if !b.take(1) || !b.take(1) || b.take(1) {
		t.Fatal("burst")
	}
	now = now.Add(100 * time.Millisecond)
	if !b.take(1) || b.take(1) {
		t.Fatal("refill")
	}
}

func TestRejectsBadToken(t *testing.T) {
	f := newFixture(t, nil)
	_, resp, err := f.dial("junk.junk")
	if err == nil || resp == nil || resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("got %v %v", resp, err)
	}
	_, resp, err = f.dial("")
	if err == nil || resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("no token: %v", err)
	}
}

func TestAddresses(t *testing.T) {
	f := newFixture(t, func(c *config) { c.MaxClients = 2 })
	want := netip.AddrFrom4([4]byte{127, f.block, 3, 4})
	_, a := f.join(claims{Adr: want.String()})
	if a != want {
		t.Fatalf("address %v, want %v", a, want)
	}
	// the same address twice is refused
	if _, resp, err := f.dial(f.token(claims{Adr: want.String()})); err == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("duplicate address: %v", err)
	}
	// outside the client network
	if _, resp, err := f.dial(f.token(claims{Adr: "127.0.1.1"})); err == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("foreign address: %v", err)
	}
	_, b := f.join(claims{})
	if !f.g.cfg.ClientNet.Contains(b) || b == a || b == f.g.cfg.Hub {
		t.Fatalf("allocated %v", b)
	}
	// max clients
	if _, resp, err := f.dial(f.token(claims{})); err == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("max clients: %v", err)
	}
}

func TestSameUserReplacesSession(t *testing.T) {
	f := newFixture(t, nil)
	want := netip.AddrFrom4([4]byte{127, f.block, 3, 5})
	old, a := f.join(claims{Sub: "u1", Adr: want.String()})
	if a != want {
		t.Fatalf("address %v, want %v", a, want)
	}
	// another user's token for the address is still refused
	if _, resp, err := f.dial(f.token(claims{Sub: "u2", Adr: want.String()})); err == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("other user: %v", err)
	}
	// the same user's newer session (a reload, a second tab) takes it over;
	// the old one reads the 1008 close (its page then gives up)
	closed := make(chan websocket.StatusCode, 1)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		for {
			if _, _, err := old.Read(ctx); err != nil {
				closed <- websocket.CloseStatus(err)
				return
			}
		}
	}()
	_, b := f.join(claims{Sub: "u1", Adr: want.String()})
	if b != want {
		t.Fatalf("replacement address %v, want %v", b, want)
	}
	select {
	case code := <-closed:
		if code != websocket.StatusPolicyViolation {
			t.Fatalf("old session closed with %v, want 1008", code)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("old session not closed")
	}
}

func TestDiscoveryAndUDP(t *testing.T) {
	f := newFixture(t, nil)
	server := listenUDP(t, f.server, 5150)
	other := listenUDP(t, netip.AddrFrom4([4]byte{127, 1, f.block, 2}), 5150) // s2: not in the token
	ws, addr := f.join(claims{Srv: []string{"s1"}})

	// a broadcast search reaches the allowed server, from the client's address and port
	write(t, ws, encodeClientUDP(netip.MustParseAddr("255.255.255.255"), 5150, 5151, []byte("search")))
	got, from := recvUDP(t, server)
	if string(got) != "search" || from != netip.AddrPortFrom(addr, 5151) {
		t.Fatalf("server got %q from %v", got, from)
	}
	other.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, _, err := other.ReadFromUDPAddrPort(make([]byte, 64)); err == nil {
		t.Fatal("a server outside the token got the broadcast")
	}

	// the server's unicast answer comes back as a UDP frame
	server.WriteToUDPAddrPort([]byte("advert"), from)
	frame := read(t, ws)
	u, err := parseUDP(frame)
	if err != nil || u.Addr != f.server || u.Port != 5150 || u.Local != 5151 || string(u.Payload) != "advert" {
		t.Fatalf("answer %+v %v", u, err)
	}

	// a server's broadcast to the hub goes to the client
	hubSender := listenUDP(t, f.server, 5151)
	hubSender.WriteToUDPAddrPort([]byte("hub"), netip.AddrPortFrom(f.g.cfg.Hub, 5151))
	u, err = parseUDP(read(t, ws))
	if err != nil || u.Addr != f.server || u.Port != 5151 || u.Local != 5151 || string(u.Payload) != "hub" {
		t.Fatalf("hub %+v %v", u, err)
	}

	// unicast to a server outside the token, or to a port that is not a game port, goes nowhere
	write(t, ws, encodeClientUDP(netip.AddrFrom4([4]byte{127, 1, f.block, 2}), 5150, 5151, []byte("no")))
	write(t, ws, encodeClientUDP(f.server, 22, 5151, []byte("no")))
	write(t, ws, encodeClientUDP(f.server, 5150, 5151, []byte("yes")))
	got, _ = recvUDP(t, server)
	if string(got) != "yes" {
		t.Fatalf("got %q", got)
	}
	other.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, _, err := other.ReadFromUDPAddrPort(make([]byte, 64)); err == nil {
		t.Fatal("unicast outside the token delivered")
	}
}

func TestStreams(t *testing.T) {
	f := newFixture(t, nil)
	ln, err := net.ListenTCP("tcp4", net.TCPAddrFromAddrPort(netip.AddrPortFrom(f.server, 5150)))
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	ws, addr := f.join(claims{})

	write(t, ws, encodeOpen(1, f.server, 5150, 49152))
	conn, err := ln.AcceptTCP()
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	if ra := conn.RemoteAddr().(*net.TCPAddr).AddrPort().Addr().Unmap(); ra != addr {
		t.Fatalf("stream from %v, want %v", ra, addr)
	}
	if f := read(t, ws); !bytes.Equal(f, encodeOpened(1, true)) {
		t.Fatalf("opened %v", f)
	}
	write(t, ws, encodeData(1, []byte("hello server")))
	buf := make([]byte, 64)
	conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	n, _ := io.ReadAtLeast(conn, buf, len("hello server"))
	if string(buf[:n]) != "hello server" {
		t.Fatalf("server read %q", buf[:n])
	}
	conn.Write([]byte("hello client"))
	id, p, _ := parseStream(read(t, ws))
	if id != 1 || string(p) != "hello client" {
		t.Fatalf("client read %d %q", id, p)
	}
	conn.Close()
	if f := read(t, ws); !bytes.Equal(f, encodeClose(1)) {
		t.Fatalf("close %v", f)
	}

	// refused: nobody listens on 5151, and 22 is not a game port
	write(t, ws, encodeOpen(2, f.server, 5151, 49153))
	if f := read(t, ws); !bytes.Equal(f, encodeOpened(2, false)) {
		t.Fatalf("refused %v", f)
	}
	write(t, ws, encodeOpen(3, f.server, 22, 49154))
	if f := read(t, ws); !bytes.Equal(f, encodeOpened(3, false)) {
		t.Fatalf("port 22 %v", f)
	}
}

func TestLimits(t *testing.T) {
	f := newFixture(t, func(c *config) {
		c.FrameRate, c.FrameBurst = 1, 5
		c.MaxFrame = 1024
	})
	server := listenUDP(t, f.server, 5150)
	ws, _ := f.join(claims{})
	for i := 0; i < 20; i++ {
		write(t, ws, encodeClientUDP(f.server, 5150, 5151, []byte{byte(i)}))
	}
	delivered := 0
	server.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	for {
		if _, _, err := server.ReadFromUDPAddrPort(make([]byte, 16)); err != nil {
			break
		}
		delivered++
	}
	if delivered < 4 || delivered > 6 {
		t.Fatalf("rate limit let %d of 20 through", delivered)
	}
	// an oversized frame closes the session
	big := encodeClientUDP(f.server, 5150, 5151, make([]byte, 2048))
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	ws.Write(ctx, websocket.MessageBinary, big)
	_, _, err := ws.Read(ctx)
	if websocket.CloseStatus(err) != websocket.StatusMessageTooBig {
		t.Fatalf("oversized frame: %v", err)
	}
}

func TestShutdownClosesSessions(t *testing.T) {
	f := newFixture(t, nil)
	ws, _ := f.join(claims{})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	go f.g.shutdown(ctx)
	_, _, err := ws.Read(ctx)
	if websocket.CloseStatus(err) != websocket.StatusGoingAway {
		t.Fatalf("close status %v", err)
	}
	if _, resp, err := f.dial(f.token(claims{})); err == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("join during shutdown: %v", err)
	}
}

var _ = binary.BigEndian

func TestPingEcho(t *testing.T) {
	f := newFixture(t, nil)
	ws, _ := f.join(claims{})
	ping := []byte{framePing, 1, 2, 3, 4, 5, 6, 7, 8}
	write(t, ws, ping)
	if got := read(t, ws); string(got) != string(ping) {
		t.Fatalf("want the ping back, got %v", got)
	}
	// an oversized token is ignored; the next ping is still answered
	write(t, ws, make([]byte, 2+maxPingToken))
	ws2 := []byte{framePing, 9}
	write(t, ws, ws2)
	if got := read(t, ws); string(got) != string(ws2) {
		t.Fatalf("want the second ping back, got %v", got)
	}
}
