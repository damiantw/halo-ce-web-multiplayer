package main

// Join tokens, issued by the Laravel app:
//
//	base64url(claims JSON) "." base64url(HMAC-SHA256(secret, first part))
//
// (base64url without padding). Claims:
//
//	sub  the user (string, for logs)
//	sid  a random session id, used once
//	exp  expiry, Unix seconds (at most maxTokenLifetime ahead)
//	srv  the server ids the session may reach; ["*"] for all
//	adr  optional: the client address to use (in the client network);
//	     without it the gateway picks one

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
	"sync"
	"time"
)

const maxTokenLifetime = 15 * time.Minute

type claims struct {
	Sub string   `json:"sub"`
	Sid string   `json:"sid"`
	Exp int64    `json:"exp"`
	Srv []string `json:"srv"`
	Adr string   `json:"adr,omitempty"`
}

var (
	errTokenFormat    = errors.New("malformed token")
	errTokenSignature = errors.New("bad token signature")
	errTokenExpired   = errors.New("token expired")
	errTokenLifetime  = errors.New("token lifetime too long")
	errTokenReplayed  = errors.New("token already used")
	errTokenClaims    = errors.New("token lacks sid or srv")
)

func signToken(secret []byte, c claims) string {
	body, _ := json.Marshal(c)
	first := base64.RawURLEncoding.EncodeToString(body)
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(first))
	return first + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

// verifier checks tokens and remembers the session ids it accepted until
// they expire, so that each token opens one session.
type verifier struct {
	secret []byte
	now    func() time.Time
	mu     sync.Mutex
	used   map[string]int64
}

func newVerifier(secret []byte) *verifier {
	return &verifier{secret: secret, now: time.Now, used: map[string]int64{}}
}

func (v *verifier) verify(token string) (claims, error) {
	var c claims
	first, sig, ok := strings.Cut(token, ".")
	if !ok || first == "" || sig == "" {
		return c, errTokenFormat
	}
	got, err := base64.RawURLEncoding.DecodeString(sig)
	if err != nil {
		return c, errTokenFormat
	}
	mac := hmac.New(sha256.New, v.secret)
	mac.Write([]byte(first))
	if !hmac.Equal(got, mac.Sum(nil)) {
		return c, errTokenSignature
	}
	body, err := base64.RawURLEncoding.DecodeString(first)
	if err != nil || json.Unmarshal(body, &c) != nil {
		return c, errTokenFormat
	}
	now := v.now().Unix()
	if c.Exp <= now {
		return c, errTokenExpired
	}
	if c.Exp > now+int64(maxTokenLifetime/time.Second) {
		return c, errTokenLifetime
	}
	if c.Sid == "" || len(c.Srv) == 0 {
		return c, errTokenClaims
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	for sid, exp := range v.used {
		if exp <= now {
			delete(v.used, sid)
		}
	}
	if _, seen := v.used[c.Sid]; seen {
		return c, errTokenReplayed
	}
	v.used[c.Sid] = c.Exp
	return c, nil
}

// tokenFromProtocols finds the token in Sec-WebSocket-Protocol: the
// client offers "halo.v1" and "t.<token>" (a query string would end up in
// access logs).
func tokenFromProtocols(header []string) string {
	for _, h := range header {
		for _, p := range strings.Split(h, ",") {
			p = strings.TrimSpace(p)
			if strings.HasPrefix(p, "t.") {
				return p[2:]
			}
		}
	}
	return ""
}
