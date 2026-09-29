package main

import (
	"fmt"
	"net/netip"
	"sort"
	"strings"
	"sync"
)

// registry holds the game servers: an id (the Laravel app's) and the
// loopback address the server's network.address is set to.
type registry struct {
	mu     sync.RWMutex
	byID   map[string]netip.Addr
	byAddr map[netip.Addr]string
}

func newRegistry() *registry {
	return &registry{byID: map[string]netip.Addr{}, byAddr: map[netip.Addr]string{}}
}

// parseServers reads "id=127.0.1.1,id2=127.0.1.2".
func parseServers(text string) (map[string]netip.Addr, error) {
	out := map[string]netip.Addr{}
	for _, item := range strings.FieldsFunc(text, func(r rune) bool { return r == ',' || r == ' ' || r == ';' }) {
		id, a, ok := strings.Cut(item, "=")
		if !ok || id == "" {
			return nil, fmt.Errorf("server %q: want id=address", item)
		}
		addr, err := netip.ParseAddr(a)
		if err != nil || !addr.Is4() {
			return nil, fmt.Errorf("server %q: bad IPv4 address", item)
		}
		out[id] = addr
	}
	return out, nil
}

func (r *registry) set(id string, addr netip.Addr) error {
	if id == "" || id == "*" {
		return fmt.Errorf("bad server id %q", id)
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if other, ok := r.byAddr[addr]; ok && other != id {
		return fmt.Errorf("address %s belongs to server %s", addr, other)
	}
	if old, ok := r.byID[id]; ok {
		delete(r.byAddr, old)
	}
	r.byID[id] = addr
	r.byAddr[addr] = id
	return nil
}

func (r *registry) remove(id string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	addr, ok := r.byID[id]
	if ok {
		delete(r.byID, id)
		delete(r.byAddr, addr)
	}
	return ok
}

func (r *registry) idOf(addr netip.Addr) (string, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	id, ok := r.byAddr[addr]
	return id, ok
}

// allowed lists the addresses of the servers a token's srv claim names.
func (r *registry) allowed(srv []string) []netip.Addr {
	r.mu.RLock()
	defer r.mu.RUnlock()
	var out []netip.Addr
	for _, id := range srv {
		if id == "*" {
			out = out[:0]
			for _, a := range r.byID {
				out = append(out, a)
			}
			break
		}
		if a, ok := r.byID[id]; ok {
			out = append(out, a)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Less(out[j]) })
	return out
}

func (r *registry) snapshot() map[string]string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := map[string]string{}
	for id, a := range r.byID {
		out[id] = a.String()
	}
	return out
}

// permits says whether a token's srv claim reaches the server at addr.
func permits(srv []string, id string) bool {
	for _, s := range srv {
		if s == "*" || s == id {
			return true
		}
	}
	return false
}
