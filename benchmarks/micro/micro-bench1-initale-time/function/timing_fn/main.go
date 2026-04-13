package main

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"strconv"

	"golang.org/x/sys/unix"
)

const benchHeaderName = "X-Bench-T0-Ns"

type response struct {
	T0NS          int64 `json:"t0_ns"`
	HeaderNS      int64 `json:"header_ns"`
	BodyDoneNS    int64 `json:"body_done_ns"`
	DeltaHeaderNS int64 `json:"delta_header_ns"`
	DeltaBodyNS   int64 `json:"delta_body_ns"`
	BodyBytesRead int64 `json:"body_bytes_read"`
	ContentLength int64 `json:"content_length"`
}

func monotonicNS() int64 {
	var ts unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &ts); err != nil {
		return 0
	}
	return ts.Nano()
}

func handler(w http.ResponseWriter, r *http.Request) {
	headerNS := monotonicNS()

	var t0 int64
	if v := r.Header.Get(benchHeaderName); v != "" {
		if parsed, err := strconv.ParseInt(v, 10, 64); err == nil {
			t0 = parsed
		}
	}

	bytesRead, _ := io.Copy(io.Discard, r.Body)
	_ = r.Body.Close()

	bodyDoneNS := monotonicNS()

	deltaHeaderNS := int64(0)
	deltaBodyNS := int64(0)
	if t0 != 0 {
		deltaHeaderNS = headerNS - t0
		deltaBodyNS = bodyDoneNS - t0
	}

	resp := response{
		T0NS:          t0,
		HeaderNS:      headerNS,
		BodyDoneNS:    bodyDoneNS,
		DeltaHeaderNS: deltaHeaderNS,
		DeltaBodyNS:   deltaBodyNS,
		BodyBytesRead: bytesRead,
		ContentLength: r.ContentLength,
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(resp)
}

func main() {
	http.HandleFunc("/", handler)

	addr := ":8080"
	log.Printf("timing_fn listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}
