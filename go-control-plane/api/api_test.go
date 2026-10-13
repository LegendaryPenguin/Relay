package api

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/LegendaryPenguin/relay/config"
	"github.com/LegendaryPenguin/relay/discovery"
)

func quiet() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

func testServer(t *testing.T, endpoints []config.Endpoint) (*httptest.Server, *Server) {
	t.Helper()
	health := discovery.NewHealthState(1, 1, quiet())
	reg := discovery.NewRegistry(health, quiet())
	reg.AddResolver(&discovery.StaticResolver{ClusterName: "api", Endpoints: endpoints})
	reg.ResolveAll(context.Background())
	reg.ProbeAll(context.Background(), func(ctx context.Context, e config.Endpoint) discovery.ProbeResult {
		return discovery.ProbeResult{Address: e.Address(), OK: true}
	}, 4)

	srv := NewServer(reg, health, []config.Route{{Cluster: "api", PathPrefix: ""}}, quiet())
	srv.Rebuild()
	ts := httptest.NewServer(srv.Routes())
	t.Cleanup(ts.Close)
	return ts, srv
}

func defaultEndpoints() []config.Endpoint {
	return []config.Endpoint{
		{Host: "10.0.0.1", Port: 8080, Weight: 1},
		{Host: "10.0.0.2", Port: 8080, Weight: 1},
		{Host: "10.0.0.3", Port: 8080, Weight: 1},
	}
}

func TestConfigEndpointServesASnapshot(t *testing.T) {
	ts, _ := testServer(t, defaultEndpoints())
	resp, err := http.Get(ts.URL + "/config")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status %d", resp.StatusCode)
	}
	var snap config.Snapshot
	if err := json.NewDecoder(resp.Body).Decode(&snap); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if snap.Version == 0 {
		t.Error("snapshot version is zero")
	}
	if len(snap.Clusters) != 1 || len(snap.Clusters[0].Endpoints) != 3 {
		t.Errorf("snapshot = %+v", snap)
	}
	if resp.Header.Get("ETag") == "" {
		t.Error("no ETag, so every poll re-downloads the whole config")
	}
}

func TestEtagAvoidsRedownloadingAnUnchangedConfig(t *testing.T) {
	// With a hundred proxies polling every second and a large endpoint list,
	// this is the difference between a trivial poll and a steady megabit of
	// pointless traffic.
	ts, _ := testServer(t, defaultEndpoints())
	resp, err := http.Get(ts.URL + "/config")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	etag := resp.Header.Get("ETag")
	resp.Body.Close()

	req, _ := http.NewRequest(http.MethodGet, ts.URL+"/config", nil)
	req.Header.Set("If-None-Match", etag)
	resp2, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("conditional get: %v", err)
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusNotModified {
		t.Fatalf("status %d, want 304", resp2.StatusCode)
	}
	body, _ := io.ReadAll(resp2.Body)
	if len(body) != 0 {
		t.Errorf("304 carried %d bytes of body", len(body))
	}
}

func TestVersionOnlyIncrementsWhenContentChanges(t *testing.T) {
	// Bumping on every rebuild would defeat the ETag and make every proxy
	// re-download an identical config on every poll, which is the whole
	// saving.
	_, srv := testServer(t, defaultEndpoints())
	first := srv.Snapshot().Version
	for i := 0; i < 5; i++ {
		srv.Rebuild()
	}
	if got := srv.Snapshot().Version; got != first {
		t.Errorf("version moved from %d to %d with no content change", first, got)
	}
}

func TestVersionIncrementsWhenEndpointsChange(t *testing.T) {
	health := discovery.NewHealthState(1, 1, quiet())
	reg := discovery.NewRegistry(health, quiet())
	resolver := &discovery.StaticResolver{ClusterName: "api", Endpoints: defaultEndpoints()}
	reg.AddResolver(resolver)
	ctx := context.Background()
	allOK := func(ctx context.Context, e config.Endpoint) discovery.ProbeResult {
		return discovery.ProbeResult{Address: e.Address(), OK: true}
	}
	reg.ResolveAll(ctx)
	reg.ProbeAll(ctx, allOK, 4)

	srv := NewServer(reg, health, []config.Route{{Cluster: "api", PathPrefix: ""}}, quiet())
	srv.Rebuild()
	before := srv.Snapshot().Version

	resolver.Endpoints = resolver.Endpoints[:2]
	reg.ResolveAll(ctx)
	reg.ProbeAll(ctx, allOK, 4)
	srv.Rebuild()

	if srv.Snapshot().Version <= before {
		t.Error("version did not advance after the endpoint set shrank")
	}
	if n := len(srv.Snapshot().Clusters[0].Endpoints); n != 2 {
		t.Errorf("snapshot has %d endpoints, want 2", n)
	}
}

func TestPutRoutesRejectsInvalidConfigAndKeepsServing(t *testing.T) {
	// The operator gets an error at the moment they made the mistake, and the
	// data plane keeps running the last valid config.
	ts, srv := testServer(t, defaultEndpoints())
	before := srv.Snapshot().Version

	body := `{"routes":[{"cluster":"nonexistent","path_prefix":""}]}`
	resp, err := http.Post(ts.URL+"/routes", "application/json", bytes.NewReader([]byte(body)))
	if err != nil {
		t.Fatalf("post: %v", err)
	}
	resp.Body.Close()
	// PUT is the route; POST should be rejected by the mux.
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("POST /routes: status %d, want 405", resp.StatusCode)
	}

	req, _ := http.NewRequest(http.MethodPut, ts.URL+"/routes", bytes.NewReader([]byte(body)))
	req.Header.Set("Content-Type", "application/json")
	resp2, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("put: %v", err)
	}
	defer resp2.Body.Close()
	if resp2.StatusCode != http.StatusBadRequest {
		t.Fatalf("status %d, want 400", resp2.StatusCode)
	}
	if srv.Snapshot().Version != before {
		t.Error("an invalid config changed the published snapshot")
	}
}

func TestPutRoutesAcceptsValidConfig(t *testing.T) {
	ts, srv := testServer(t, defaultEndpoints())
	body := `{"routes":[{"cluster":"api","path_prefix":"/v1","balancer":"ring_hash","hash_source":"path"}]}`
	req, _ := http.NewRequest(http.MethodPut, ts.URL+"/routes", bytes.NewReader([]byte(body)))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("put: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(resp.Body)
		t.Fatalf("status %d: %s", resp.StatusCode, b)
	}
	snap := srv.Snapshot()
	if len(snap.Routes) != 1 || snap.Routes[0].Balancer != config.RingHash {
		t.Errorf("routes = %+v", snap.Routes)
	}
	// Defaults must have been filled in.
	if snap.Routes[0].Retry.MaxAttempts == 0 || snap.Routes[0].Retry.Jitter == 0 {
		t.Errorf("defaults not applied: %+v", snap.Routes[0].Retry)
	}
}

func TestPutRoutesRejectsUnknownFields(t *testing.T) {
	// Version skew should break loudly rather than leaving an operator
	// believing they configured something they did not.
	ts, _ := testServer(t, defaultEndpoints())
	body := `{"routes":[{"cluster":"api","path_prefix":"","future_knob":true}]}`
	req, _ := http.NewRequest(http.MethodPut, ts.URL+"/routes", bytes.NewReader([]byte(body)))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("put: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("status %d, want 400", resp.StatusCode)
	}
}

func TestStatusReportsWarningsAndResolveErrors(t *testing.T) {
	ts, _ := testServer(t, defaultEndpoints())
	resp, err := http.Get(ts.URL + "/status")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	var out struct {
		Version       uint64            `json:"version"`
		Clusters      []map[string]any  `json:"clusters"`
		Routes        int               `json:"routes"`
		Stats         map[string]uint64 `json:"stats"`
		Warnings      []string          `json:"warnings"`
		ResolveErrors map[string]string `json:"resolve_errors"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if out.Routes != 1 || len(out.Clusters) != 1 {
		t.Errorf("status = %+v", out)
	}
	if out.Clusters[0]["eligible"].(float64) != 3 {
		t.Errorf("eligible = %v, want 3", out.Clusters[0]["eligible"])
	}
	if len(out.ResolveErrors) != 0 {
		t.Errorf("unexpected resolve errors: %+v", out.ResolveErrors)
	}
}

func TestPlacementIsPredictableAndStable(t *testing.T) {
	// The point of the control plane computing the same hash: being able to
	// ask where a key goes without tailing proxy logs.
	ts, _ := testServer(t, defaultEndpoints())

	query := func(key string) string {
		resp, err := http.Get(ts.URL + "/placement?cluster=api&key=" + key)
		if err != nil {
			t.Fatalf("get: %v", err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			b, _ := io.ReadAll(resp.Body)
			t.Fatalf("status %d: %s", resp.StatusCode, b)
		}
		var out struct {
			Endpoint string `json:"endpoint"`
			RingSize int    `json:"ring_size"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
			t.Fatalf("decode: %v", err)
		}
		if out.RingSize != 3*160 {
			t.Errorf("ring size = %d, want 480", out.RingSize)
		}
		return out.Endpoint
	}

	first := query("/user/42")
	for i := 0; i < 5; i++ {
		if got := query("/user/42"); got != first {
			t.Fatalf("placement moved: %s then %s", first, got)
		}
	}

	// Distinct keys must spread across the cluster rather than collapsing.
	seen := map[string]bool{}
	for i := 0; i < 60; i++ {
		seen[query("/key/"+string(rune('a'+i%26))+string(rune('a'+i/26)))] = true
	}
	if len(seen) < 2 {
		t.Errorf("all keys landed on one endpoint: %v", seen)
	}
}

func TestPlacementRequiresBothParameters(t *testing.T) {
	ts, _ := testServer(t, defaultEndpoints())
	for _, path := range []string{"/placement", "/placement?key=x", "/placement?cluster=api"} {
		resp, err := http.Get(ts.URL + path)
		if err != nil {
			t.Fatalf("get %s: %v", path, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusBadRequest {
			t.Errorf("%s: status %d, want 400", path, resp.StatusCode)
		}
	}
}

func TestPlacementOnAnEmptyClusterIs503(t *testing.T) {
	health := discovery.NewHealthState(1, 1, quiet())
	reg := discovery.NewRegistry(health, quiet())
	reg.AddResolver(&discovery.StaticResolver{ClusterName: "api", Endpoints: defaultEndpoints()})
	ctx := context.Background()
	reg.ResolveAll(ctx)
	// Probe everything as failing.
	reg.ProbeAll(ctx, func(ctx context.Context, e config.Endpoint) discovery.ProbeResult {
		return discovery.ProbeResult{Address: e.Address(), OK: false}
	}, 4)

	srv := NewServer(reg, health, []config.Route{{Cluster: "api", PathPrefix: ""}}, quiet())
	srv.Rebuild()
	ts := httptest.NewServer(srv.Routes())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/placement?cluster=api&key=x")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Errorf("status %d, want 503", resp.StatusCode)
	}
}

func TestRingMatchesTheDataPlanesConstruction(t *testing.T) {
	// Same key format, same hash, same weight scaling. A divergence makes the
	// control plane's placement answers confidently wrong.
	c := config.Cluster{Name: "api", Endpoints: []config.Endpoint{
		{Host: "10.0.0.1", Port: 8080, Weight: 1, Health: config.HealthHealthy},
		{Host: "10.0.0.2", Port: 8080, Weight: 4, Health: config.HealthHealthy},
		{Host: "10.0.0.3", Port: 8080, Weight: 1, Health: config.HealthUnhealthy},
	}}
	ring := BuildRing(c, 160)
	// Unhealthy endpoints contribute nothing; weight scales points.
	if want := 160 * (1 + 4); ring.Size() != want {
		t.Errorf("ring size = %d, want %d", ring.Size(), want)
	}

	// The weighted endpoint should own roughly 4/5 of the keyspace.
	counts := map[int]int{}
	for i := 0; i < 20000; i++ {
		idx, ok := ring.Lookup(config.Fnv1a64("obj-" + string(rune(i%256)) + string(rune(i/256))))
		if !ok {
			t.Fatal("lookup failed on a non-empty ring")
		}
		counts[idx]++
	}
	share := float64(counts[1]) / 20000
	if share < 0.68 || share > 0.92 {
		t.Errorf("weighted endpoint share = %.2f, want ~0.80", share)
	}
}

func TestEmptyRingLookupFails(t *testing.T) {
	ring := BuildRing(config.Cluster{Name: "empty"}, 160)
	if _, ok := ring.Lookup(123); ok {
		t.Error("an empty ring returned an endpoint")
	}
}
