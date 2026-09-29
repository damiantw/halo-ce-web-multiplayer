package main

// The frames of port/web/src/posix_web_net.c: one frame per WebSocket
// binary message, big-endian numbers, IPv4 addresses as 4 bytes.
//
//	client -> gateway                      gateway -> client
//	1 UDP    dst ip4, dst port2, src port2, payload
//	                                       1 UDP    src ip4, src port2, dst port2, payload
//	2 OPEN   stream4, dst ip4, dst port2, src port2
//	                                       2 OPEN   (unused: no inbound streams yet)
//	3 OPENED (unused)                      3 OPENED stream4, ok1
//	4 DATA   stream4, payload              4 DATA   stream4, payload
//	5 CLOSE  stream4                       5 CLOSE  stream4
//	                                       6 HELLO  address ip4

import (
	"encoding/binary"
	"errors"
	"net/netip"
)

const (
	frameUDP    = 1
	frameOpen   = 2
	frameOpened = 3
	frameData   = 4
	frameClose  = 5
	frameHello  = 6
)

var errShortFrame = errors.New("short frame")

type udpFrame struct {
	Addr    netip.Addr // destination (from a client) or source (to a client)
	Port    uint16     // its port
	Local   uint16     // the client's port: source (from a client) or destination (to a client)
	Payload []byte
}

type openFrame struct {
	Stream  uint32
	Addr    netip.Addr
	Port    uint16
	SrcPort uint16
}

func addr4(b []byte) netip.Addr { return netip.AddrFrom4([4]byte{b[0], b[1], b[2], b[3]}) }

func parseUDP(f []byte) (udpFrame, error) {
	if len(f) < 9 || f[0] != frameUDP {
		return udpFrame{}, errShortFrame
	}
	return udpFrame{
		Addr:    addr4(f[1:5]),
		Port:    binary.BigEndian.Uint16(f[5:7]),
		Local:   binary.BigEndian.Uint16(f[7:9]),
		Payload: f[9:],
	}, nil
}

func parseOpen(f []byte) (openFrame, error) {
	if len(f) < 13 || f[0] != frameOpen {
		return openFrame{}, errShortFrame
	}
	return openFrame{
		Stream:  binary.BigEndian.Uint32(f[1:5]),
		Addr:    addr4(f[5:9]),
		Port:    binary.BigEndian.Uint16(f[9:11]),
		SrcPort: binary.BigEndian.Uint16(f[11:13]),
	}, nil
}

func parseStream(f []byte) (uint32, []byte, error) {
	if len(f) < 5 {
		return 0, nil, errShortFrame
	}
	return binary.BigEndian.Uint32(f[1:5]), f[5:], nil
}

// encodeUDP builds a gateway -> client UDP frame: from src:srcPort to the
// client's port dstPort.
func encodeUDP(src netip.Addr, srcPort, dstPort uint16, payload []byte) []byte {
	f := make([]byte, 9+len(payload))
	f[0] = frameUDP
	a := src.As4()
	copy(f[1:5], a[:])
	binary.BigEndian.PutUint16(f[5:7], srcPort)
	binary.BigEndian.PutUint16(f[7:9], dstPort)
	copy(f[9:], payload)
	return f
}

func encodeClientUDP(dst netip.Addr, dstPort, srcPort uint16, payload []byte) []byte {
	// the same layout, read the other way round
	return encodeUDP(dst, dstPort, srcPort, payload)
}

func encodeOpen(stream uint32, dst netip.Addr, dstPort, srcPort uint16) []byte {
	f := make([]byte, 13)
	f[0] = frameOpen
	binary.BigEndian.PutUint32(f[1:5], stream)
	a := dst.As4()
	copy(f[5:9], a[:])
	binary.BigEndian.PutUint16(f[9:11], dstPort)
	binary.BigEndian.PutUint16(f[11:13], srcPort)
	return f
}

func encodeOpened(stream uint32, ok bool) []byte {
	f := []byte{frameOpened, 0, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(f[1:5], stream)
	if ok {
		f[5] = 1
	}
	return f
}

func encodeData(stream uint32, payload []byte) []byte {
	f := make([]byte, 5+len(payload))
	f[0] = frameData
	binary.BigEndian.PutUint32(f[1:5], stream)
	copy(f[5:], payload)
	return f
}

func encodeClose(stream uint32) []byte {
	f := []byte{frameClose, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(f[1:5], stream)
	return f
}

func encodeHello(a netip.Addr) []byte {
	b := a.As4()
	return []byte{frameHello, b[0], b[1], b[2], b[3]}
}
