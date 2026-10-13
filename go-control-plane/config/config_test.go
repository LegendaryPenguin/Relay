package config

import (
	"strings"
	"testing"
)

func validSnapshot() Snapshot {
	return Snapshot{
		Clusters: []Cluster{{
			Name: "api",
			Endpoints: []Endpoint{
				{Host: "10.0.0.1", Port: 8080, Weight: 1, Health: HealthHealthy},
				{Host: "10.0.0.2", Port: 8080, Weight: 1, Health: HealthHealthy},
			},
		}},
		Routes: []Route{{Cluster: "api", PathPrefix: "", Balancer: P2C}},
	}
}

func TestValidSnapshotPasses(t *testing.T) {
	s := validSnapshot()
	s.Normalize()
	if err := s.Validate(); err != nil {
		t.Fatalf("valid snapshot rejected: %v", err)
	}
}

func TestRejectsRouteToUnknownCluster(t *testing.T) {
	// A route to a nonexistent cluster is a 404 for every matching request,
	// and the data plane cannot tell that from a deliberate catch-all.
	s := validSnapshot()
	s.Routes[0].Cluster = "nope"
	s.Normalize()
	if err := s.Validate(); err == nil {
		t.Fatal("accepted a route to an unknown cluster")
	}
}

func TestRejectsDuplicateEndpoint(t *testing.T) {
	// A duplicate gets double the ring points and double the round-robin
	// slots, so it silently receives twice the traffic.
	s := validSnapshot()
	s.Clusters[0].Endpoints = append(s.Clusters[0].Endpoints, s.Clusters[0].Endpoints[0])
	s.Normalize()
	if err := s.Validate(); err == nil {
		t.Fatal("accepted a duplicated endpoint")
	}
}

func TestRejectsDuplicatePathPrefix(t *testing.T) {
	// One of the two would be unreachable, and which one depends on sort
	// order — so the config does something its author cannot predict.
	s := validSnapshot()
	s.Routes = append(s.Routes, Route{Cluster: "api", PathPrefix: "", Balancer: P2C})
	s.Normalize()
	if err := s.Validate(); err == nil {
		t.Fatal("accepted two routes with the same prefix")
	}
}

func TestRejectsRingHashWithoutAKey(t *testing.T) {
	// Ring hashing with no key hashes everything to zero and sends the whole
	// cluster's traffic to one endpoint.
	s := validSnapshot()
	s.Routes[0].Balancer = RingHash
	s.Routes[0].HashSource = HashNone
	// Normalize would fill in a default, so validate the raw value.
	if err := s.Routes[0].Validate(); err == nil {
		t.Fatal("accepted ring_hash with hash_source none")
	}
}

func TestRejectsHeaderHashWithoutAHeaderName(t *testing.T) {
	r := Route{Cluster: "api", Balancer: RingHash, HashSource: HashHeader}
	if err := r.Validate(); err == nil {
		t.Fatal("accepted header hashing with no header named")
	}
}

func TestRejectsPerAttemptTimeoutExceedingDeadline(t *testing.T) {
	// The data plane clamps, but silently clamping a timeout is how a config
	// ends up not doing what its author believes.
	r := Route{Cluster: "api", Balancer: P2C, PerAttemptTimeoutMs: 5000, OverallDeadlineMs: 1000}
	if err := r.Validate(); err == nil {
		t.Fatal("accepted a per-attempt timeout longer than the overall deadline")
	}
}

func TestRejectsOutOfRangeSoftThreshold(t *testing.T) {
	for _, v := range []float64{-0.1, 1.5} {
		r := Route{Cluster: "api", Balancer: P2C, Shedder: ShedderConfig{SoftThreshold: v}}
		if err := r.Validate(); err == nil {
			t.Errorf("accepted soft_threshold %.2f", v)
		}
	}
}

func TestNormalizeFillsConservativeDefaults(t *testing.T) {
	// Every default is the conservative choice. A proxy whose defaults are
	// generous is one that amplifies its first outage.
	s := Snapshot{
		Clusters: []Cluster{{Name: "api", Endpoints: []Endpoint{{Host: "h", Port: 1}}}},
		Routes:   []Route{{Cluster: "api"}},
	}
	s.Normalize()
	r := s.Routes[0]

	if r.Balancer != P2C {
		t.Errorf("default balancer = %q, want p2c", r.Balancer)
	}
	if r.Retry.MaxAttempts != 3 {
		t.Errorf("default max attempts = %d, want 3", r.Retry.MaxAttempts)
	}
	if r.Retry.BudgetRatio != 0.2 {
		t.Errorf("default budget ratio = %.2f, want 0.2", r.Retry.BudgetRatio)
	}
	// Jitter must never default to zero: without it every client that failed
	// together retries together.
	if r.Retry.Jitter == 0 {
		t.Error("jitter defaulted to zero")
	}
	if r.Shedder.MaxQueueDepth == 0 || r.Shedder.MaxConcurrent == 0 {
		t.Error("shedder limits defaulted to zero, which disables shedding")
	}
	// An endpoint with no stated weight or health is an omission, not a drain.
	if s.Clusters[0].Endpoints[0].Weight != 1 {
		t.Errorf("default weight = %d, want 1", s.Clusters[0].Endpoints[0].Weight)
	}
	if s.Clusters[0].Endpoints[0].Health != HealthUnknown {
		t.Errorf("default health = %q, want unknown", s.Clusters[0].Endpoints[0].Health)
	}
}

func TestNormalizeRespectsAnExplicitDrain(t *testing.T) {
	// Weight zero alongside an explicit health is a deliberate drain and must
	// survive normalization.
	s := Snapshot{
		Clusters: []Cluster{{Name: "api", Endpoints: []Endpoint{
			{Host: "h", Port: 1, Weight: 0, Health: HealthHealthy},
		}}},
		Routes: []Route{{Cluster: "api"}},
	}
	s.Normalize()
	if s.Clusters[0].Endpoints[0].Weight != 0 {
		t.Error("an explicit drain was overwritten")
	}
}

func TestNormalizeSortsRoutesLongestPrefixFirst(t *testing.T) {
	// Matching the data plane's own ordering, so the published snapshot reads
	// the way it will behave.
	s := Snapshot{
		Clusters: []Cluster{{Name: "api", Endpoints: []Endpoint{{Host: "h", Port: 1}}}},
		Routes: []Route{
			{Cluster: "api", PathPrefix: "/a"},
			{Cluster: "api", PathPrefix: "/a/b/c"},
			{Cluster: "api", PathPrefix: ""},
			{Cluster: "api", PathPrefix: "/a/b"},
		},
	}
	s.Normalize()
	want := []string{"/a/b/c", "/a/b", "/a", ""}
	for i, w := range want {
		if s.Routes[i].PathPrefix != w {
			t.Errorf("route %d prefix = %q, want %q", i, s.Routes[i].PathPrefix, w)
		}
	}
}

func TestWarningsAreAdvisoryNotFatal(t *testing.T) {
	// A cluster with no healthy endpoints is almost certainly a mistake — and
	// it is also exactly what a snapshot looks like mid-redeploy. Refusing to
	// publish then would pin the data plane to the pre-deploy endpoint list,
	// which is worse.
	s := Snapshot{
		Clusters: []Cluster{{Name: "api", Endpoints: []Endpoint{
			{Host: "h", Port: 1, Weight: 1, Health: HealthUnhealthy},
		}}},
		Routes: []Route{{Cluster: "api", PathPrefix: ""}},
	}
	s.Normalize()
	if err := s.Validate(); err != nil {
		t.Fatalf("an all-unhealthy cluster must still validate: %v", err)
	}
	warnings := s.Warnings()
	if len(warnings) == 0 {
		t.Fatal("no warning for a cluster with nothing eligible")
	}
	found := false
	for _, w := range warnings {
		if strings.Contains(w, "503") {
			found = true
		}
	}
	if !found {
		t.Errorf("warnings do not mention the consequence: %v", warnings)
	}
}

func TestWarnsAboutAMissingCatchAll(t *testing.T) {
	s := validSnapshot()
	s.Routes[0].PathPrefix = "/api"
	s.Normalize()
	found := false
	for _, w := range s.Warnings() {
		if strings.Contains(w, "catch-all") {
			found = true
		}
	}
	if !found {
		t.Errorf("no catch-all warning: %v", s.Warnings())
	}
}

func TestWarnsAboutAnUnreferencedCluster(t *testing.T) {
	s := validSnapshot()
	s.Clusters = append(s.Clusters, Cluster{
		Name:      "orphan",
		Endpoints: []Endpoint{{Host: "h", Port: 2, Weight: 1, Health: HealthHealthy}},
	})
	s.Normalize()
	found := false
	for _, w := range s.Warnings() {
		if strings.Contains(w, "orphan") {
			found = true
		}
	}
	if !found {
		t.Errorf("no warning for an unreferenced cluster: %v", s.Warnings())
	}
}

func TestEligibleCount(t *testing.T) {
	c := Cluster{Endpoints: []Endpoint{
		{Host: "a", Port: 1, Weight: 1, Health: HealthHealthy},
		{Host: "b", Port: 1, Weight: 1, Health: HealthUnhealthy},
		{Host: "c", Port: 1, Weight: 1, Health: HealthUnknown},
		{Host: "d", Port: 1, Weight: 0, Health: HealthHealthy},
	}}
	// Only the healthy, non-drained one counts. Unknown is not eligible:
	// sending traffic to a host nothing has probed is sending it on faith.
	if n := c.EligibleCount(); n != 1 {
		t.Errorf("eligible = %d, want 1", n)
	}
}

func TestHashMatchesThePinnedVector(t *testing.T) {
	// The cross-language contract. The C++ side asserts the identical value;
	// a divergence makes the control plane's placement answers confidently
	// wrong, which is worse than not having the endpoint at all.
	if got := Fnv1a64(""); got != 17280346270528514342 {
		t.Errorf("Fnv1a64(\"\") = %d, want 17280346270528514342", got)
	}
	// The finalizer must be present: the raw FNV-1a offset basis coming back
	// means it was dropped and the ring degrades to a handful of giant arcs.
	if Fnv1a64("") == 0xcbf29ce484222325 {
		t.Error("the MurmurHash3 finalizer is missing")
	}
	if Fnv1a64("a") == Fnv1a64("b") {
		t.Error("hash collision on single characters")
	}
}

func TestEndpointValidation(t *testing.T) {
	cases := []struct {
		name string
		e    Endpoint
		ok   bool
	}{
		{"valid", Endpoint{Host: "h", Port: 1, Health: HealthHealthy}, true},
		{"no host", Endpoint{Port: 1, Health: HealthHealthy}, false},
		{"no port", Endpoint{Host: "h", Health: HealthHealthy}, false},
		{"bad health", Endpoint{Host: "h", Port: 1, Health: Health("sick")}, false},
	}
	for _, tc := range cases {
		err := tc.e.Validate()
		if (err == nil) != tc.ok {
			t.Errorf("%s: err = %v, want ok=%v", tc.name, err, tc.ok)
		}
	}
}
