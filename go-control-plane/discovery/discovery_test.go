package discovery

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	"github.com/LegendaryPenguin/relay/config"
)

func quiet() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

var t0 = time.Date(2025, 7, 1, 12, 0, 0, 0, time.UTC)

func TestHealthHysteresisInBothDirections(t *testing.T) {
	// A single failed probe must not evict a host — one dropped packet would
	// do it. A single success must not restore one either, or a flapping host
	// oscillates, which is worse for clients than being consistently out.
	h := NewHealthState(3, 2, quiet())
	addr := "10.0.0.1:8080"

	// Two failures: still not unhealthy.
	for i := 0; i < 2; i++ {
		got, _ := h.Record(ProbeResult{Address: addr, OK: false}, t0)
		if got == config.HealthUnhealthy {
			t.Fatalf("marked unhealthy after %d failures, threshold is 3", i+1)
		}
	}
	// Third failure crosses it.
	if got, changed := h.Record(ProbeResult{Address: addr, OK: false}, t0); got != config.HealthUnhealthy || !changed {
		t.Fatalf("health = %q changed = %v after 3 failures", got, changed)
	}

	// One success does not restore.
	if got, _ := h.Record(ProbeResult{Address: addr, OK: true}, t0); got != config.HealthUnhealthy {
		t.Errorf("restored after a single success: %q", got)
	}
	// Two does.
	if got, changed := h.Record(ProbeResult{Address: addr, OK: true}, t0); got != config.HealthHealthy || !changed {
		t.Errorf("health = %q changed = %v after 2 successes", got, changed)
	}
}

func TestASuccessResetsTheFailureRun(t *testing.T) {
	// Consecutive, not cumulative: an endpoint failing one probe in three
	// forever should not eventually be evicted.
	h := NewHealthState(3, 2, quiet())
	addr := "a:1"
	for round := 0; round < 20; round++ {
		h.Record(ProbeResult{Address: addr, OK: false}, t0)
		h.Record(ProbeResult{Address: addr, OK: false}, t0)
		h.Record(ProbeResult{Address: addr, OK: true}, t0)
	}
	if got := h.Health(addr); got == config.HealthUnhealthy {
		t.Error("an intermittently failing endpoint was evicted by a consecutive-failure rule")
	}
}

func TestUnknownUntilProbed(t *testing.T) {
	h := NewHealthState(3, 2, quiet())
	if got := h.Health("never-seen:1"); got != config.HealthUnknown {
		t.Errorf("health = %q, want unknown", got)
	}
}

func TestForgetDropsStaleStateSoReaddedHostsStartUnknown(t *testing.T) {
	// A re-added address inheriting a previous host's verdict is the subtle
	// bug: a host that was unhealthy an hour ago and has since been replaced
	// should start Unknown, not Unhealthy.
	h := NewHealthState(1, 1, quiet())
	h.Record(ProbeResult{Address: "gone:1", OK: false}, t0)
	if h.Health("gone:1") != config.HealthUnhealthy {
		t.Fatal("setup failed")
	}

	removed := h.Forget(map[string]bool{"other:1": true})
	if removed != 1 {
		t.Errorf("forgot %d entries, want 1", removed)
	}
	if got := h.Health("gone:1"); got != config.HealthUnknown {
		t.Errorf("a re-added address inherited stale health: %q", got)
	}
}

func TestResolveErrorKeepsTheLastGoodList(t *testing.T) {
	// A resolver failure is not evidence that the endpoints are gone.
	// Treating it as such turns a DNS blip into a total outage.
	health := NewHealthState(1, 1, quiet())
	reg := NewRegistry(health, quiet())

	failing := &flakyResolver{name: "api", endpoints: []config.Endpoint{
		{Host: "10.0.0.1", Port: 8080, Weight: 1},
	}}
	reg.AddResolver(failing)

	ctx := context.Background()
	reg.ResolveAll(ctx)
	if n := len(reg.Clusters()[0].Endpoints); n != 1 {
		t.Fatalf("first resolution produced %d endpoints", n)
	}

	// Now DNS breaks.
	failing.fail = true
	reg.ResolveAll(ctx)
	clusters := reg.Clusters()
	if len(clusters) != 1 || len(clusters[0].Endpoints) != 1 {
		t.Fatalf("a resolver failure emptied the cluster: %+v", clusters)
	}
	if errs := reg.ResolveErrors(); len(errs) != 1 {
		t.Errorf("the stale state is not reported: %+v", errs)
	}

	// And it recovers.
	failing.fail = false
	reg.ResolveAll(ctx)
	if errs := reg.ResolveErrors(); len(errs) != 0 {
		t.Errorf("errors persisted after recovery: %+v", errs)
	}
}

type flakyResolver struct {
	name      string
	endpoints []config.Endpoint
	fail      bool
}

func (f *flakyResolver) Name() string { return f.name }
func (f *flakyResolver) Resolve(ctx context.Context) ([]config.Endpoint, error) {
	if f.fail {
		return nil, errors.New("simulated DNS failure")
	}
	return f.endpoints, nil
}

func TestDNSResolverMarksNewEndpointsUnknown(t *testing.T) {
	// Marking them healthy would send traffic to a host nothing has checked.
	r := &DNSResolver{
		ClusterName: "api", Host: "svc.local", Port: 8080, Weight: 2, Zone: "z1",
		Lookup: func(ctx context.Context, host string) ([]net.IP, error) {
			return []net.IP{net.ParseIP("10.0.0.2"), net.ParseIP("10.0.0.1")}, nil
		},
	}
	got, err := r.Resolve(context.Background())
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("got %d endpoints", len(got))
	}
	// Sorted, so a snapshot is stable across resolutions and does not
	// republish just because DNS reordered its answer.
	if got[0].Host != "10.0.0.1" {
		t.Errorf("endpoints not sorted: %+v", got)
	}
	for _, e := range got {
		if e.Health != config.HealthUnknown {
			t.Errorf("endpoint %s starts %q, want unknown", e.Address(), e.Health)
		}
		if e.Weight != 2 {
			t.Errorf("weight = %d, want 2", e.Weight)
		}
		if e.Zone != "z1" {
			t.Errorf("zone = %q", e.Zone)
		}
	}
}

func TestDNSResolverEmptyAnswerIsAnError(t *testing.T) {
	// Distinguished from a lookup failure by the caller; both keep the last
	// good list, but the operator needs to be able to tell them apart.
	r := &DNSResolver{
		ClusterName: "api", Host: "svc.local", Port: 8080,
		Lookup: func(ctx context.Context, host string) ([]net.IP, error) {
			return nil, nil
		},
	}
	if _, err := r.Resolve(context.Background()); err == nil {
		t.Fatal("an empty DNS answer was accepted as a resolution")
	}
}

func TestProbeAllAppliesHealthToTheClusterList(t *testing.T) {
	health := NewHealthState(1, 1, quiet())
	reg := NewRegistry(health, quiet())
	reg.AddResolver(&StaticResolver{ClusterName: "api", Endpoints: []config.Endpoint{
		{Host: "10.0.0.1", Port: 8080, Weight: 1},
		{Host: "10.0.0.2", Port: 8080, Weight: 1},
	}})

	ctx := context.Background()
	reg.ResolveAll(ctx)

	// 10.0.0.1 is healthy, 10.0.0.2 is not.
	probe := func(ctx context.Context, e config.Endpoint) ProbeResult {
		return ProbeResult{Address: e.Address(), OK: e.Host == "10.0.0.1"}
	}
	reg.ProbeAll(ctx, probe, 4)

	clusters := reg.Clusters()
	if len(clusters) != 1 {
		t.Fatalf("got %d clusters", len(clusters))
	}
	if clusters[0].EligibleCount() != 1 {
		t.Errorf("eligible = %d, want 1: %+v", clusters[0].EligibleCount(), clusters[0].Endpoints)
	}
}

func TestProbeAllForgetsDepartedEndpoints(t *testing.T) {
	health := NewHealthState(1, 1, quiet())
	reg := NewRegistry(health, quiet())
	resolver := &StaticResolver{ClusterName: "api", Endpoints: []config.Endpoint{
		{Host: "10.0.0.1", Port: 8080, Weight: 1},
		{Host: "10.0.0.2", Port: 8080, Weight: 1},
	}}
	reg.AddResolver(resolver)
	ctx := context.Background()

	alwaysOK := func(ctx context.Context, e config.Endpoint) ProbeResult {
		return ProbeResult{Address: e.Address(), OK: true}
	}
	reg.ResolveAll(ctx)
	reg.ProbeAll(ctx, alwaysOK, 4)
	if len(health.Stats(t0)) != 2 {
		t.Fatalf("expected 2 tracked endpoints, got %d", len(health.Stats(t0)))
	}

	// One endpoint goes away.
	resolver.Endpoints = resolver.Endpoints[:1]
	reg.ResolveAll(ctx)
	reg.ProbeAll(ctx, alwaysOK, 4)
	if n := len(health.Stats(t0)); n != 1 {
		t.Errorf("health state retains %d endpoints after one departed", n)
	}
}

func TestProbeAllIsConcurrentButBounded(t *testing.T) {
	health := NewHealthState(1, 1, quiet())
	reg := NewRegistry(health, quiet())
	var endpoints []config.Endpoint
	for i := 0; i < 50; i++ {
		endpoints = append(endpoints, config.Endpoint{
			Host: fmt.Sprintf("10.0.0.%d", i+1), Port: 8080, Weight: 1,
		})
	}
	reg.AddResolver(&StaticResolver{ClusterName: "api", Endpoints: endpoints})
	ctx := context.Background()
	reg.ResolveAll(ctx)

	// Run under -race: the probes write into shared health state.
	reg.ProbeAll(ctx, func(ctx context.Context, e config.Endpoint) ProbeResult {
		return ProbeResult{Address: e.Address(), OK: true}
	}, 8)

	if n := len(health.Stats(t0)); n != 50 {
		t.Errorf("probed %d of 50 endpoints", n)
	}
}

func TestStatsAreSortedAndCounted(t *testing.T) {
	h := NewHealthState(1, 1, quiet())
	h.Record(ProbeResult{Address: "b:1", OK: true, Latency: 2 * time.Millisecond}, t0)
	h.Record(ProbeResult{Address: "a:1", OK: false}, t0)
	h.Record(ProbeResult{Address: "a:1", OK: false}, t0)

	stats := h.Stats(t0.Add(time.Second))
	if len(stats) != 2 || stats[0].Address != "a:1" {
		t.Fatalf("stats not sorted: %+v", stats)
	}
	if stats[0].TotalProbes != 2 || stats[0].TotalFailures != 2 {
		t.Errorf("a:1 counters = %+v", stats[0])
	}
	if stats[1].LastLatencyMs != 2 {
		t.Errorf("b:1 latency = %.1fms, want 2", stats[1].LastLatencyMs)
	}
}
