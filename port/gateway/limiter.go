package main

import (
	"sync"
	"time"
)

// bucket is a token bucket: rate tokens a second, up to burst.
type bucket struct {
	mu     sync.Mutex
	rate   float64
	burst  float64
	tokens float64
	last   time.Time
	now    func() time.Time
}

func newBucket(rate, burst float64) *bucket {
	return &bucket{rate: rate, burst: burst, tokens: burst, now: time.Now}
}

func (b *bucket) take(n float64) bool {
	if b == nil || b.rate <= 0 {
		return true
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	now := b.now()
	if !b.last.IsZero() {
		b.tokens += now.Sub(b.last).Seconds() * b.rate
		if b.tokens > b.burst {
			b.tokens = b.burst
		}
	}
	b.last = now
	if b.tokens < n {
		return false
	}
	b.tokens -= n
	return true
}
