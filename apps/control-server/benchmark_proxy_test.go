package main

import (
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/http/httputil"
	"strings"
	"testing"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (fn roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return fn(request)
}

func benchmarkProxyTestServer(t *testing.T, upstreamURL string) *Server {
	t.Helper()
	return NewServer(
		Config{DataDir: t.TempDir(), WebDir: t.TempDir(), BenchmarkURL: upstreamURL},
		slog.New(slog.NewTextHandler(io.Discard, nil)),
	)
}

func setBenchmarkTransport(t *testing.T, server *Server, transport http.RoundTripper) {
	t.Helper()
	proxy, ok := server.benchmarkProxy.(*httputil.ReverseProxy)
	if !ok {
		t.Fatalf("unexpected benchmark proxy type %T", server.benchmarkProxy)
	}
	proxy.Transport = transport
}

func TestBenchmarkProxyForwardsRequestAndResponse(t *testing.T) {
	var gotMethod, gotPath, gotQuery, gotBody, gotContentType string
	server := benchmarkProxyTestServer(t, "http://benchmark.test")
	setBenchmarkTransport(t, server, roundTripFunc(func(r *http.Request) (*http.Response, error) {
		gotMethod = r.Method
		gotPath = r.URL.Path
		gotQuery = r.URL.RawQuery
		gotContentType = r.Header.Get("Content-Type")
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatal(err)
		}
		gotBody = string(body)
		return &http.Response{
			StatusCode: http.StatusAccepted,
			Header: http.Header{
				"Content-Type": []string{"application/json"},
				"Location":     []string{"/api/benchmarks/bench_test"},
			},
			Body: io.NopCloser(strings.NewReader(`{"jobId":"bench_test","status":"queued"}`)),
		}, nil
	}))
	request := httptest.NewRequest(
		http.MethodPost,
		"/api/benchmarks?source=web",
		strings.NewReader(`{"name":"smoke"}`),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)

	if response.Code != http.StatusAccepted {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	if gotMethod != http.MethodPost || gotPath != "/api/benchmarks" || gotQuery != "source=web" {
		t.Fatalf("unexpected upstream request: %s %s?%s", gotMethod, gotPath, gotQuery)
	}
	if gotContentType != "application/json" || gotBody != `{"name":"smoke"}` {
		t.Fatalf("unexpected upstream payload: content-type=%q body=%q", gotContentType, gotBody)
	}
	if response.Header().Get("Location") != "/api/benchmarks/bench_test" ||
		!strings.Contains(response.Body.String(), `"jobId":"bench_test"`) {
		t.Fatalf("upstream response was not preserved: headers=%v body=%s", response.Header(), response.Body.String())
	}
}

func TestBenchmarkProxyPreservesUpstreamError(t *testing.T) {
	server := benchmarkProxyTestServer(t, "http://benchmark.test")
	setBenchmarkTransport(t, server, roundTripFunc(func(*http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusTooManyRequests,
			Header:     http.Header{"Content-Type": []string{"application/json"}},
			Body:       io.NopCloser(strings.NewReader(`{"error":"benchmark queue is full"}`)),
		}, nil
	}))
	request := httptest.NewRequest(http.MethodPost, "/api/benchmarks", strings.NewReader("{}"))
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)

	if response.Code != http.StatusTooManyRequests ||
		response.Body.String() != `{"error":"benchmark queue is full"}` {
		t.Fatalf("unexpected response %d: %s", response.Code, response.Body.String())
	}
}

func TestBenchmarkProxyReturnsJSONWhenUpstreamUnavailable(t *testing.T) {
	server := benchmarkProxyTestServer(t, "http://benchmark.test")
	setBenchmarkTransport(t, server, roundTripFunc(func(*http.Request) (*http.Response, error) {
		return nil, errors.New("dial refused")
	}))
	request := httptest.NewRequest(http.MethodGet, "/api/benchmarks/bench_missing/result", nil)
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)

	if response.Code != http.StatusBadGateway {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	if !strings.HasPrefix(response.Header().Get("Content-Type"), "application/json") ||
		!strings.Contains(response.Body.String(), "benchmark service is unavailable") {
		t.Fatalf("unexpected response: headers=%v body=%s", response.Header(), response.Body.String())
	}
}

func TestBenchmarkProxyRejectsInvalidConfiguration(t *testing.T) {
	for _, rawURL := range []string{
		"file:///tmp/benchmark.sock",
		"http://user:secret@benchmark.test",
		"http://benchmark.test?token=secret",
	} {
		t.Run(rawURL, func(t *testing.T) {
			server := benchmarkProxyTestServer(t, rawURL)
			request := httptest.NewRequest(http.MethodGet, "/api/benchmarks", nil)
			response := httptest.NewRecorder()
			server.routes().ServeHTTP(response, request)

			if response.Code != http.StatusServiceUnavailable ||
				!strings.Contains(response.Body.String(), "benchmark service is not configured") {
				t.Fatalf("unexpected response %d: %s", response.Code, response.Body.String())
			}
		})
	}
}
