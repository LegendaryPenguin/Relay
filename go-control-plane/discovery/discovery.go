// Package discovery resolves endpoints and decides which are healthy.
//
// Two responsibilities that sound like one and behave very differently:
//
//	Resolution  which endpoints exist. Comes from DNS or a static list, and
//	            changes when someone deploys.
//	Health      which of them work. Comes from active probes, and changes when
//	            something breaks.
//
// Keeping them apart matters because they fail differently. If DNS goes away,
// the right behaviour is to keep the last known endpoint list — a resolver
// failure is not evidence that the endpoints are gone, and treating it as such
// turns a DNS blip into a total outage. If a *probe* fails, that is direct
// evidence about that endpoint and should act immediately.
//
// So: resolution failures are sticky-on-last-good, health failures are
// immediate. Conflating them gives you a proxy that empties its endpoint list
// when its DNS server hiccups.
package discovery

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"sort"
	"sync"
	"time"

	"github.com/LegendaryPenguin/relay/config"
)

// Resolver turns a cluster definition into a concrete endpoint list.
type Resolver interface {
	Resolve(ctx context.Context) ([]config.Endpoint, error)
	Name() string
}

// StaticResolver returns a fixed list. Used for tests and for clusters whose
// membership is managed externally.
type StaticResolver struct {
	ClusterName string
	Endpoints   []config.Endpoint
}

func (s *StaticResolver) Name() string { return s.ClusterName }

func (s *StaticResolver) Resolve(ctx context.Context) ([]config.Endpoint, error) {
	out := make([]config.Endpoint, len(s.Endpoints))
	copy(out, s.Endpoints)
	return out, nil
}

// DNSResolver expands a hostname to every A/AAAA record.
type DNSResolver struct {
	ClusterName string
	Host        string
	Port        uint16
	Weight      uint32
	Zone        string
	Lookup      func(ctx context.Context, host string) ([]net.IP, error)
}

func (d *DNSResolver) Name() string { return d.ClusterName }

func (d *DNSResolver) Resolve(ctx context.Context) ([]config.Endpoint, error) {
	lookup := d.Lookup
	if lookup == nil {
		lookup = func(ctx context.Context, host string) ([]net.IP, error) {
			return net.DefaultResolver.LookupIP(ctx, "ip4", host)
		}
	}
	ips, err := lookup(ctx, d.Host)
	if err != nil {
		return nil, fmt.Errorf("resolve %s: %w", d.Host, err)
	}
	if len(ips) == 0 {
		// An empty answer is distinguished from an error on purpose. It is a
		// successful resolution saying the service has no instances, which is
		// a real state during a scale-to-zero — but it is also what a
		// misconfigured record looks like, so the caller decides whether to
		// trust it. Returning an error here would make the two
		// indistinguishable.
		return nil, fmt.Errorf("resolve %s: no addresses", d.Host)
	}

	weight := d.Weight
	if weight == 0 {
		weight = 1
	}
	out := make([]config.Endpoint, 0, len(ips))
	for _, ip := range ips {
		out = append(out, config.Endpoint{
			Host:   ip.String(),
			Port:   d.Port,
			Weight: weight,
			// Newly resolved endpoints are Unknown, not Healthy. Marking them
			// healthy would send traffic to a host nothing has checked; the
			// health checker promotes them on the first successful probe.
			Health: config.HealthUnknown,
			Zone:   d.Zone,
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Address() < out[j].Address() })
	return out, nil
}

// ProbeResult is one health-check outcome.
type ProbeResult struct {
	Address string
	OK      bool
	Err     error
	Latency time.Duration
}

// Prober checks one endpoint.
type Prober func(ctx context.Context, e config.Endpoint) ProbeResult

// HTTPProber issues a GET and treats 2xx/3xx as healthy.
func HTTPProber(path string, timeout time.Duration) Prober {
	client := &http.Client{
		Timeout: timeout,
		Transport: &http.Transport{
			// A dedicated connection per probe. Reusing a pooled connection
			// would mean a probe can succeed against a connection established
			// before the endpoint broke, which is exactly the thing the probe
			// is supposed to detect.
			DisableKeepAlives: true,
		},
	}
	return func(ctx context.Context, e config.Endpoint) ProbeResult {
		start := time.Now()
		url := fmt.Sprintf("http://%s%s", e.Address(), path)
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
		if err != nil {
			return ProbeResult{Address: e.Address(), Err: err}
		}
		resp, err := client.Do(req)
		latency := time.Since(start)
		if err != nil {
			return ProbeResult{Address: e.Address(), Err: err, Latency: latency}
		}
		defer resp.Body.Close()
		ok := resp.StatusCode >= 200 && resp.StatusCode < 400
		var rerr error
		if !ok {
			rerr = fmt.Errorf("probe returned %d", resp.StatusCode)
		}
		return ProbeResult{Address: e.Address(), OK: ok, Err: rerr, Latency: latency}
	}
}

// HealthState tracks consecutive outcomes per endpoint.
//
// Hysteresis in both directions, with deliberately asymmetric thresholds:
// going unhealthy is fast and coming back is slow. A single failed probe
// should not evict a host (one dropped packet would), and a single successful
// probe should not restore one (a flapping host would oscillate, which is
// worse for clients than being consistently out).
type HealthState struct {
	mu sync.Mutex
	// UnhealthyThreshold consecutive failures before marking unhealthy.
	UnhealthyThreshold int
	// HealthyThreshold consecutive successes before marking healthy. Higher
	// than the unhealthy one on purpose.
	HealthyThreshold int

	state map[string]*endpointHealth
	log   *slog.Logger
}

type endpointHealth struct {
	health            config.Health
	consecutiveOK     int
	consecutiveFailed int
	lastChange        time.Time
	totalProbes       uint64
	totalFailures     uint64
	lastLatency       time.Duration
}

func NewHealthState(unhealthyThreshold, healthyThreshold int, logger *slog.Logger) *HealthState {
	if unhealthyThreshold <= 0 {
		unhealthyThreshold = 3
	}
	if healthyThreshold <= 0 {
		healthyThreshold = 2
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &HealthState{
		UnhealthyThreshold: unhealthyThreshold,
		HealthyThreshold:   healthyThreshold,
		state:              map[string]*endpointHealth{},
		log:                logger,
	}
}

// Record applies a probe result and reports whether the endpoint's health
// changed.
func (h *HealthState) Record(r ProbeResult, now time.Time) (config.Health, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()

	st, ok := h.state[r.Address]
	if !ok {
		st = &endpointHealth{health: config.HealthUnknown, lastChange: now}
		h.state[r.Address] = st
	}
	st.totalProbes++
	st.lastLatency = r.Latency

	previous := st.health
	if r.OK {
		st.consecutiveOK++
		st.consecutiveFailed = 0
		if st.consecutiveOK >= h.HealthyThreshold {
			st.health = config.HealthHealthy
		}
	} else {
		st.totalFailures++
		st.consecutiveFailed++
		st.consecutiveOK = 0
		if st.consecutiveFailed >= h.UnhealthyThreshold {
			st.health = config.HealthUnhealthy
		}
	}

	changed := st.health != previous
	if changed {
		st.lastChange = now
		h.log.Info("endpoint health changed",
			"endpoint", r.Address, "from", previous, "to", st.health,
			"consecutive_ok", st.consecutiveOK, "consecutive_failed", st.consecutiveFailed)
	}
	return st.health, changed
}

// Health returns the current verdict for an address.
func (h *HealthState) Health(address string) config.Health {
	h.mu.Lock()
	defer h.mu.Unlock()
	st, ok := h.state[address]
	if !ok {
		return config.HealthUnknown
	}
	return st.health
}

// Forget drops state for endpoints no longer in the resolved set.
//
// Without this the map grows forever as endpoints come and go, and — worse —
// a re-added endpoint would inherit stale health from a previous incarnation.
// A host that was unhealthy an hour ago and has since been replaced should
// start Unknown, not Unhealthy.
func (h *HealthState) Forget(keep map[string]bool) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	removed := 0
	for addr := range h.state {
		if !keep[addr] {
			delete(h.state, addr)
			removed++
		}
	}
	return removed
}

// Stats is per-endpoint health detail for the operator API.
type Stats struct {
	Address        string        `json:"address"`
	Health         config.Health `json:"health"`
	TotalProbes    uint64        `json:"total_probes"`
	TotalFailures  uint64        `json:"total_failures"`
	LastLatencyMs  float64       `json:"last_latency_ms"`
	SecondsInState float64       `json:"seconds_in_state"`
}

func (h *HealthState) Stats(now time.Time) []Stats {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make([]Stats, 0, len(h.state))
	for addr, st := range h.state {
		out = append(out, Stats{
			Address:        addr,
			Health:         st.health,
			TotalProbes:    st.totalProbes,
			TotalFailures:  st.totalFailures,
			LastLatencyMs:  float64(st.lastLatency.Microseconds()) / 1000.0,
			SecondsInState: now.Sub(st.lastChange).Seconds(),
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Address < out[j].Address })
	return out
}

// Registry owns resolution and health for every cluster.
type Registry struct {
	mu        sync.Mutex
	resolvers map[string]Resolver
	// lastGood is the most recent successful resolution per cluster.
	//
	// The sticky part. A resolver failure is not evidence that the endpoints
	// are gone, so the last known list is kept rather than publishing an
	// empty cluster and turning a DNS blip into a total outage.
	lastGood   map[string][]config.Endpoint
	resolveErr map[string]error
	health     *HealthState
	log        *slog.Logger
}

func NewRegistry(health *HealthState, logger *slog.Logger) *Registry {
	if logger == nil {
		logger = slog.Default()
	}
	return &Registry{
		resolvers:  map[string]Resolver{},
		lastGood:   map[string][]config.Endpoint{},
		resolveErr: map[string]error{},
		health:     health,
		log:        logger,
	}
}

func (r *Registry) AddResolver(res Resolver) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.resolvers[res.Name()] = res
}

func (r *Registry) ClusterNames() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]string, 0, len(r.resolvers))
	for name := range r.resolvers {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// ResolveAll refreshes every cluster's endpoint list.
func (r *Registry) ResolveAll(ctx context.Context) {
	for _, name := range r.ClusterNames() {
		r.mu.Lock()
		res := r.resolvers[name]
		r.mu.Unlock()
		if res == nil {
			continue
		}

		endpoints, err := res.Resolve(ctx)
		r.mu.Lock()
		if err != nil {
			r.resolveErr[name] = err
			// Keep the last good list. Logged at warn because serving a stale
			// endpoint set is a real degradation even though it is the right
			// call.
			if _, have := r.lastGood[name]; have {
				r.log.Warn("resolution failed; keeping the last known endpoints",
					"cluster", name, "error", err, "endpoints", len(r.lastGood[name]))
			} else {
				r.log.Error("resolution failed and there is no previous result",
					"cluster", name, "error", err)
			}
		} else {
			delete(r.resolveErr, name)
			r.lastGood[name] = endpoints
		}
		r.mu.Unlock()
	}
}

// ProbeAll health-checks every known endpoint, concurrently.
func (r *Registry) ProbeAll(ctx context.Context, probe Prober, concurrency int) {
	if concurrency <= 0 {
		concurrency = 16
	}

	r.mu.Lock()
	var all []config.Endpoint
	keep := map[string]bool{}
	for _, endpoints := range r.lastGood {
		for _, e := range endpoints {
			all = append(all, e)
			keep[e.Address()] = true
		}
	}
	r.mu.Unlock()

	// Endpoints that have gone away lose their health state, so a re-added
	// address starts Unknown rather than inheriting a previous host's verdict.
	r.health.Forget(keep)

	sem := make(chan struct{}, concurrency)
	var wg sync.WaitGroup
	now := time.Now()
	for _, e := range all {
		wg.Add(1)
		sem <- struct{}{}
		go func(e config.Endpoint) {
			defer wg.Done()
			defer func() { <-sem }()
			r.health.Record(probe(ctx, e), now)
		}(e)
	}
	wg.Wait()
}

// Clusters builds the current cluster list, applying health.
func (r *Registry) Clusters() []config.Cluster {
	r.mu.Lock()
	names := make([]string, 0, len(r.lastGood))
	for name := range r.lastGood {
		names = append(names, name)
	}
	sort.Strings(names)
	lists := make(map[string][]config.Endpoint, len(names))
	for _, name := range names {
		lists[name] = append([]config.Endpoint(nil), r.lastGood[name]...)
	}
	r.mu.Unlock()

	out := make([]config.Cluster, 0, len(names))
	for _, name := range names {
		endpoints := lists[name]
		for i := range endpoints {
			endpoints[i].Health = r.health.Health(endpoints[i].Address())
		}
		out = append(out, config.Cluster{Name: name, Endpoints: endpoints})
	}
	return out
}

// ResolveErrors reports clusters currently serving a stale endpoint list.
func (r *Registry) ResolveErrors() map[string]string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := map[string]string{}
	for name, err := range r.resolveErr {
		out[name] = err.Error()
	}
	return out
}
