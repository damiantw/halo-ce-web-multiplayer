// halo-gateway bridges the web build's WebSocket (port/web/src/
// posix_web_net.c) to the system link servers on this machine's loopback
// addresses. docs/gateway.md describes it.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"net/netip"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
)

func env(name, fallback string) string {
	if v, ok := os.LookupEnv(name); ok {
		return v
	}
	return fallback
}

func loadConfig() (config, string, string, string, error) {
	cfg := defaultConfig()
	listen := env("HALO_GATEWAY_LISTEN", "127.0.0.1:7780")
	control := env("HALO_GATEWAY_CONTROL", "127.0.0.1:7781")
	servers := env("HALO_GATEWAY_SERVERS", "")
	cfg.Secret = []byte(os.Getenv("HALO_GATEWAY_SECRET"))
	if len(cfg.Secret) < 16 {
		return cfg, "", "", "", errors.New("HALO_GATEWAY_SECRET must be set (at least 16 bytes)")
	}
	var err error
	if v := os.Getenv("HALO_GATEWAY_CLIENT_NET"); v != "" {
		if cfg.ClientNet, err = netip.ParsePrefix(v); err != nil {
			return cfg, "", "", "", fmt.Errorf("HALO_GATEWAY_CLIENT_NET: %w", err)
		}
	}
	if v := os.Getenv("HALO_GATEWAY_HUB"); v != "" {
		if cfg.Hub, err = netip.ParseAddr(v); err != nil {
			return cfg, "", "", "", fmt.Errorf("HALO_GATEWAY_HUB: %w", err)
		}
	}
	ints := map[string]*int{"HALO_GATEWAY_MAX_CLIENTS": &cfg.MaxClients, "HALO_GATEWAY_MAX_FRAME": &cfg.MaxFrame}
	for name, p := range ints {
		if v := os.Getenv(name); v != "" {
			if *p, err = strconv.Atoi(v); err != nil {
				return cfg, "", "", "", fmt.Errorf("%s: %w", name, err)
			}
		}
	}
	floats := map[string]*float64{"HALO_GATEWAY_FRAME_RATE": &cfg.FrameRate, "HALO_GATEWAY_BYTE_RATE": &cfg.ByteRate}
	for name, p := range floats {
		if v := os.Getenv(name); v != "" {
			if *p, err = strconv.ParseFloat(v, 64); err != nil {
				return cfg, "", "", "", fmt.Errorf("%s: %w", name, err)
			}
			if name == "HALO_GATEWAY_FRAME_RATE" {
				cfg.FrameBurst = 2 * *p
			} else {
				cfg.ByteBurst = 2 * *p
			}
		}
	}
	cfg.RTC.Listen = os.Getenv("HALO_GATEWAY_RTC_LISTEN")
	if cfg.RTC.Listen == "off" {
		cfg.RTC.Listen = ""
	}
	for _, v := range strings.Split(os.Getenv("HALO_GATEWAY_RTC_PUBLIC_IPS"), ",") {
		if v = strings.TrimSpace(v); v == "" {
			continue
		}
		a, err := netip.ParseAddr(v)
		if err != nil {
			return cfg, "", "", "", fmt.Errorf("HALO_GATEWAY_RTC_PUBLIC_IPS: %w", err)
		}
		cfg.RTC.PublicIPs = append(cfg.RTC.PublicIPs, a)
	}
	if v := os.Getenv("HALO_GATEWAY_RTC_PORT"); v != "" {
		port, err := strconv.ParseUint(v, 10, 16)
		if err != nil {
			return cfg, "", "", "", fmt.Errorf("HALO_GATEWAY_RTC_PORT: %w", err)
		}
		cfg.RTC.Port = uint16(port)
	}
	return cfg, listen, control, servers, nil
}

// controlHandler is the daemon's local API: register and remove servers,
// read the sessions and counters.
func controlHandler(g *gateway) http.Handler {
	mux := http.NewServeMux()
	writeJSON := func(w http.ResponseWriter, v any) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(v)
	}
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, map[string]bool{"ok": true}) })
	mux.HandleFunc("GET /servers", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, g.reg.snapshot()) })
	mux.HandleFunc("PUT /servers/{id}", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Address string `json:"address"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "want {\"address\":\"127.0.1.N\"}", http.StatusBadRequest)
			return
		}
		a, err := netip.ParseAddr(body.Address)
		if err != nil || !a.Is4() || g.cfg.ClientNet.Contains(a) {
			http.Error(w, "bad server address", http.StatusBadRequest)
			return
		}
		if err := g.reg.set(r.PathValue("id"), a); err != nil {
			http.Error(w, err.Error(), http.StatusConflict)
			return
		}
		g.log.Info("server registered", "server", r.PathValue("id"), "address", a.String())
		writeJSON(w, g.reg.snapshot())
	})
	mux.HandleFunc("DELETE /servers/{id}", func(w http.ResponseWriter, r *http.Request) {
		if !g.reg.remove(r.PathValue("id")) {
			http.NotFound(w, r)
			return
		}
		g.log.Info("server removed", "server", r.PathValue("id"))
		writeJSON(w, g.reg.snapshot())
	})
	mux.HandleFunc("GET /sessions", func(w http.ResponseWriter, r *http.Request) {
		type row struct {
			Sub, Sid, Addr               string
			Servers                      []string
			FramesIn, FramesOut, Dropped int64
			Transport                    string // "rtc" while the data channel carries the datagrams, else "ws"
		}
		var out []row
		for _, s := range g.sessionList() {
			out = append(out, row{s.claims.Sub, s.claims.Sid, s.addr.String(), s.claims.Srv, s.framesIn.Load(), s.framesOut.Load(), s.dropped.Load(), s.transport()})
		}
		writeJSON(w, out)
	})
	mux.HandleFunc("GET /metrics", func(w http.ResponseWriter, r *http.Request) {
		sessions, rtc := g.sessionList(), int64(0)
		for _, s := range sessions {
			if s.transport() == "rtc" {
				rtc++
			}
		}
		writeJSON(w, map[string]int64{
			"sessions": int64(len(sessions)), "accepted": g.stats.accepted.Load(), "rejected": g.stats.rejected.Load(),
			"frames_in": g.stats.framesIn.Load(), "frames_out": g.stats.framesOut.Load(), "dropped": g.stats.dropped.Load(),
			"rtc_sessions": rtc, "rtc_opened": g.stats.rtcOpened.Load(), "rtc_fallbacks": g.stats.rtcFallbacks.Load(),
		})
	})
	return mux
}

func main() {
	level := flag.String("log-level", env("HALO_GATEWAY_LOG_LEVEL", "info"), "debug, info, warn or error")
	mintSub := flag.String("mint", "", "print a join token for this user (development; uses HALO_GATEWAY_SECRET) and exit")
	mintAddr := flag.String("mint-address", "", "the token's client address (with -mint)")
	mintServers := flag.String("mint-servers", "*", "the token's server ids, comma-separated (with -mint)")
	mintTTL := flag.Duration("mint-ttl", 10*time.Minute, "the token's lifetime (with -mint)")
	flag.Parse()

	var lv slog.Level
	if err := lv.UnmarshalText([]byte(*level)); err != nil {
		fmt.Fprintln(os.Stderr, "bad -log-level:", err)
		os.Exit(2)
	}
	log := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: lv}))

	cfg, listen, control, servers, err := loadConfig()
	if err != nil {
		log.Error("configuration", "err", err.Error())
		os.Exit(2)
	}
	if *mintSub != "" {
		fmt.Println(signToken(cfg.Secret, claims{
			Sub: *mintSub, Sid: strconv.FormatInt(time.Now().UnixNano(), 36), Exp: time.Now().Add(*mintTTL).Unix(),
			Srv: strings.Split(*mintServers, ","), Adr: *mintAddr,
		}))
		return
	}
	reg := newRegistry()
	initial, err := parseServers(servers)
	if err != nil {
		log.Error("HALO_GATEWAY_SERVERS", "err", err.Error())
		os.Exit(2)
	}
	for id, a := range initial {
		if err := reg.set(id, a); err != nil {
			log.Error("HALO_GATEWAY_SERVERS", "err", err.Error())
			os.Exit(2)
		}
	}
	g := newGateway(cfg, log, reg)
	if err := g.start(); err != nil {
		log.Error("start", "err", err.Error())
		os.Exit(1)
	}
	mux := http.NewServeMux()
	mux.Handle("/gateway", g)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("ok\n")) })
	public := &http.Server{Addr: listen, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	private := &http.Server{Addr: control, Handler: controlHandler(g), ReadHeaderTimeout: 10 * time.Second}
	errs := make(chan error, 2)
	go func() { errs <- public.ListenAndServe() }()
	if control != "" && control != "off" {
		go func() { errs <- private.ListenAndServe() }()
	}
	rtc := "off"
	if g.rtc != nil {
		rtc = fmt.Sprintf("udp %s, announced %v port %d", g.rtc.conn.LocalAddr(), cfg.RTC.PublicIPs, g.rtc.cfg.Port)
	}
	log.Info("gateway listening", "listen", listen, "control", control, "hub", cfg.Hub.String(),
		"client_net", cfg.ClientNet.String(), "servers", reg.snapshot(), "max_clients", cfg.MaxClients, "rtc", rtc)

	stop := make(chan os.Signal, 2)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	select {
	case sig := <-stop:
		log.Info("shutting down", "signal", sig.String())
	case err := <-errs:
		log.Error("server failed", "err", err.Error())
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	go func() { <-stop; log.Warn("second signal: exiting now"); os.Exit(1) }()
	public.Shutdown(ctx) // stops new connections; hijacked WebSockets continue
	g.shutdown(ctx)
	private.Shutdown(ctx)
	log.Info("stopped")
}
