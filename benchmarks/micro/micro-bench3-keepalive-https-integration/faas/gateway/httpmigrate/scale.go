//go:build linux

// Package httpmigrate — scale.go
//
// Scale-from-zero for the prototype (sendfd) fast path.
//
// In vanilla mode every /function/<name> request travels through the gorilla/mux
// router, where the OpenFaaS scale-from-zero middleware (handlers.MakeScalingHandler
// → scaling.FunctionScaler) brings a 0-replica function up before the request is
// served. In prototype mode the migrate loops (RunLoop / RunLoopHTTPS) peek the
// request line and hand the raw fd straight to the container, INTERCEPTING it
// before the router — so that middleware never runs and a stopped container is
// never restarted.
//
// To restore vanilla-equivalent behaviour without re-routing the whole request
// through the router, main.go wires a ScaleFunc (backed by the SAME gateway
// FunctionScaler) into the migrate loops. The loops call it just before handing
// the fd to the container.
//
// IMPORTANT — scope of the guarantee: this only covers the FIRST request of each
// connection (the one that triggers the migration). Once the fd is migrated,
// subsequent keep-alive requests flow DIRECTLY between the client and the
// container and never traverse the gateway again, so a container that dies
// mid-session is not auto-recovered until the client opens a new connection.
// That is an inherent property of the sendfd data path, not a bug.

package httpmigrate

import (
	"fmt"
	"net/http"
	"syscall"
)

// ScaleFunc brings a function up from zero replicas and blocks until at least one
// replica is available, or returns an error. functionName is the raw name parsed
// from the request path (it may include a ".<namespace>" suffix, resolved by the
// callback). A nil ScaleFunc disables the step entirely, preserving the previous
// behaviour (used when scale_from_zero is off, and always on the vanilla path).
type ScaleFunc func(functionName string) error

// writeHTTPError writes a minimal plaintext HTTP/1.1 error response on a raw,
// UNENCRYPTED fd (the HTTP migrate path). It lets the client receive a proper
// status (e.g. 503) instead of a silently closed socket when scale-from-zero
// fails. The caller still owns and closes the fd afterwards.
//
// Do NOT use this on the HTTPS path: there the fd carries a live wolfSSL session,
// so a plaintext write would be undecodable garbage to the client. The HTTPS path
// closes the fd on failure instead.
func writeHTTPError(fd int, status int, msg string) {
	body := msg + "\n"
	resp := fmt.Sprintf(
		"HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
		status, http.StatusText(status), len(body), body,
	)
	_, _ = syscall.Write(fd, []byte(resp))
}
