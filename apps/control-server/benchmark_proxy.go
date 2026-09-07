package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"time"
)

const defaultBenchmarkURL = "http://127.0.0.1:8090"

type dependencyHealth struct {
	OK     bool   `json:"ok"`
	Status string `json:"status"`
}

type benchmarkHealthChecker struct {
	endpoint *url.URL
	client   *http.Client
}

func parseBenchmarkTarget(rawURL string) (*url.URL, error) {
	target, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || (target.Scheme != "http" && target.Scheme != "https") ||
		target.Host == "" || target.User != nil || target.RawQuery != "" || target.Fragment != "" {
		return nil, errors.New("invalid benchmark service URL")
	}
	return target, nil
}

func newBenchmarkProxy(rawURL string, logger *slog.Logger) http.Handler {
	target, err := parseBenchmarkTarget(rawURL)
	if err != nil {
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

func newBenchmarkHealthChecker(rawURL string) *benchmarkHealthChecker {
	target, err := parseBenchmarkTarget(rawURL)
	if err != nil {
		return &benchmarkHealthChecker{}
	}
	healthURL := *target
	healthURL.Path = strings.TrimRight(healthURL.Path, "/") + "/health"
	healthURL.RawPath = ""
	return &benchmarkHealthChecker{
		endpoint: &healthURL,
		client:   &http.Client{},
	}
}

func (checker *benchmarkHealthChecker) check(ctx context.Context) dependencyHealth {
	if checker == nil || checker.endpoint == nil || checker.client == nil {
		return dependencyHealth{Status: "misconfigured"}
	}
	requestContext, cancel := context.WithTimeout(ctx, 750*time.Millisecond)
	defer cancel()
	request, err := http.NewRequestWithContext(
		requestContext, http.MethodGet, checker.endpoint.String(), nil)
	if err != nil {
		return dependencyHealth{Status: "misconfigured"}
	}
	response, err := checker.client.Do(request)
	if err != nil {
		return dependencyHealth{Status: "unavailable"}
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return dependencyHealth{Status: "unavailable"}
	}
	var payload struct {
		OK bool `json:"ok"`
	}
	if err := json.NewDecoder(io.LimitReader(response.Body, 4<<10)).Decode(&payload); err != nil || !payload.OK {
		return dependencyHealth{Status: "unavailable"}
	}
	return dependencyHealth{OK: true, Status: "online"}
}
