package main

import (
	"log/slog"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"time"
)

const defaultBenchmarkURL = "http://127.0.0.1:8090"

func newBenchmarkProxy(rawURL string, logger *slog.Logger) http.Handler {
	target, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || (target.Scheme != "http" && target.Scheme != "https") ||
		target.Host == "" || target.User != nil || target.RawQuery != "" || target.Fragment != "" {
		// Do not log rawURL: a malformed URL may contain credentials.
		logger.Error("invalid benchmark service URL")
		return http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			writeError(w, http.StatusServiceUnavailable, "benchmark service is not configured")
		})
	}

	transport := http.DefaultTransport
	if defaultTransport, ok := http.DefaultTransport.(*http.Transport); ok {
		clonedTransport := defaultTransport.Clone()
		clonedTransport.ResponseHeaderTimeout = 15 * time.Second
		clonedTransport.MaxIdleConnsPerHost = 8
		transport = clonedTransport
	}

	proxy := httputil.NewSingleHostReverseProxy(target)
	proxy.Transport = transport
	proxy.ErrorHandler = func(w http.ResponseWriter, r *http.Request, proxyErr error) {
		logger.Warn(
			"benchmark service request failed",
			"method", r.Method,
			"path", r.URL.Path,
			"error", proxyErr,
		)
		writeError(w, http.StatusBadGateway, "benchmark service is unavailable")
	}
	return proxy
}
