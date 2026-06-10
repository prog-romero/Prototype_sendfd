// Copyright (c) OpenFaaS Author(s) 2021. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

package handlers

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"

	"github.com/containerd/containerd"
	"github.com/containerd/containerd/namespaces"
	faasd "github.com/openfaas/faasd/pkg"
	cninetwork "github.com/openfaas/faasd/pkg/cninetwork"
)

// MakeFunctionIPHandler returns the CNI IP address of a running function container.
//
// Route: GET /system/function-ip/{name}
// Response 200: {"ip":"10.62.0.5","name":"timing-fn-a"}
// Response 404: container not found or IP not yet assigned
// Response 503: container exists but task is not running
//
// Register this handler BEFORE calling bootstrap.Serve() by using bootstrap.Router():
//
//	bootstrap.Router().HandleFunc("/system/function-ip/{name}",
//	    handlers.MakeFunctionIPHandler(client)).Methods(http.MethodGet)
func MakeFunctionIPHandler(client *containerd.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		// Extract the function name from the request path.
		// The gorilla/mux variable would be "{name}", but we also handle
		// plain http.HandleFunc registration via prefix stripping.
		name := r.URL.Path
		name = strings.TrimPrefix(name, "/system/function-ip/")
		name = strings.Trim(name, "/")

		log.Printf("[function-ip] incoming request: path=%s resolved-name=%q\n", r.URL.Path, name)

		if name == "" {
			log.Printf("[function-ip] ERROR: empty function name in path=%s\n", r.URL.Path)
			http.Error(w, "function name is required in path", http.StatusBadRequest)
			return
		}

		ctx := namespaces.WithNamespace(context.Background(), faasd.DefaultFunctionNamespace)

		c, err := client.LoadContainer(ctx, name)
		if err != nil {
			log.Printf("[function-ip] container not found: %q — %v\n", name, err)
			http.Error(w, fmt.Sprintf("container not found: %s", name), http.StatusNotFound)
			return
		}

		task, err := c.Task(ctx, nil)
		if err != nil {
			log.Printf("[function-ip] task not running for: %q — %v\n", name, err)
			http.Error(w, fmt.Sprintf("function not running: %s", name), http.StatusServiceUnavailable)
			return
		}

		ip, err := cninetwork.GetIPAddress(name, task.Pid())
		if err != nil {
			log.Printf("[function-ip] IP not found for: %q — %v\n", name, err)
			http.Error(w, fmt.Sprintf("IP not available for: %s", name), http.StatusNotFound)
			return
		}

		log.Printf("[function-ip] OK fn=%s ip=%s\n", name, ip)
		w.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(w).Encode(map[string]string{
			"ip":   ip,
			"name": name,
		}); err != nil {
			log.Printf("[function-ip] encode error: %v\n", err)
		}
	}
}
