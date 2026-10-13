// Package config is the control plane's model of the data plane's world.
//
// The division of labour between the two halves is the whole reason Relay is
// two languages, and it comes down to one observation: the request path and
// the configuration path have opposite requirements.
//
// The data plane is on the request path. It needs predictable microseconds,
// no allocation it can avoid, and no lock on anything a request touches. That
// is what C++ is for here.
//
// The control plane is not on the request path at all. It resolves DNS, probes
// health, watches for config changes, and decides what the endpoint set should
// be — work measured in seconds, involving retries and partial failure, that
// changes shape as requirements change. Go is better at that, and more
// importantly a bug here costs a stale endpoint list rather than a latency
// spike on every request.
//
// What crosses between them is a *snapshot*: a complete, versioned, immutable
// description of clusters and routes. Never a delta. Deltas require both sides
// to agree about the state the delta applies to, and they stop agreeing the
// first time a message is lost — after which the data plane's view is wrong
// and nothing detects it. A whole snapshot is idempotent, so a lost push costs
// one round of staleness and self-corrects.
package config

import (
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"
)

// Health mirrors the data plane's enum. Three states, not a boolean: a newly
// discovered endpoint has not been probed, and treating "unknown" as healthy
// sends traffic on faith while treating it as unhealthy means a fresh
// endpoint is never probed by real requests.
type Health string

const (
	HealthUnknown   Health = "unknown"
	HealthHealthy   Health = "healthy"
	HealthUnhealthy Health = "unhealthy"
)

type Endpoint struct {
	Host string `json:"host"`
	Port uint16 `json:"port"`
	// Weight scales traffic. Zero means drain: keeps existing connections,
	// receives nothing new.
	Weight uint32 `json:"weight"`
	Health Health `json:"health"`
	Zone   string `json:"zone"`
}

func (e Endpoint) Address() string {
	return fmt.Sprintf("%s:%d", e.Host, e.Port)
}

func (e Endpoint) Validate() error {
	if e.Host == "" {
		return errors.New("endpoint has no host")
	}
	if e.Port == 0 {
		return fmt.Errorf("endpoint %s has no port", e.Host)
	}
	switch e.Health {
	case HealthUnknown, HealthHealthy, HealthUnhealthy:
	default:
		return fmt.Errorf("endpoint %s has unknown health %q", e.Address(), e.Health)
	}
	return nil
}

type BalancerKind string

const (
	RoundRobin   BalancerKind = "round_robin"
	LeastRequest BalancerKind = "least_request"
	P2C          BalancerKind = "p2c"
	Random       BalancerKind = "random"
	RingHash     BalancerKind = "ring_hash"
)

func (b BalancerKind) Valid() bool {
	switch b {
	case RoundRobin, LeastRequest, P2C, Random, RingHash:
		return true
	}
	return false
}

type HashSource string

const (
	HashNone          HashSource = "none"
	HashPath          HashSource = "path"
	HashHeader        HashSource = "header"
	HashRemoteAddress HashSource = "remote_address"
)

type Cluster struct {
	Name      string     `json:"name"`
	Endpoints []Endpoint `json:"endpoints"`
}

func (c Cluster) Validate() error {
	if c.Name == "" {
		return errors.New("cluster has no name")
	}
	seen := map[string]bool{}
	for _, e := range c.Endpoints {
		if err := e.Validate(); err != nil {
			return fmt.Errorf("cluster %s: %w", c.Name, err)
		}
		if seen[e.Address()] {
			// A duplicate endpoint would get double the ring points and
			// double the round-robin slots, so it would silently receive
			// twice the traffic.
			return fmt.Errorf("cluster %s lists %s twice", c.Name, e.Address())
		}
		seen[e.Address()] = true
	}
	return nil
}

// EligibleCount is how many endpoints can take new traffic.
//
// Worth computing here because a snapshot with zero eligible endpoints is the
// single most damaging thing the control plane can publish — every request to
// that cluster becomes a 503 — and it is worth refusing to publish rather
// than discovering it from the error rate.
func (c Cluster) EligibleCount() int {
	n := 0
	for _, e := range c.Endpoints {
		if e.Health == HealthHealthy && e.Weight > 0 {
			n++
		}
	}
	return n
}

type RetryConfig struct {
	MaxAttempts      uint32        `json:"max_attempts"`
	InitialBackoffMs int64         `json:"initial_backoff_ms"`
	MaxBackoffMs     int64         `json:"max_backoff_ms"`
	Multiplier       float64       `json:"multiplier"`
	Jitter           float64       `json:"jitter"`
	BudgetRatio      float64       `json:"budget_ratio"`
	BudgetMinPerSec  float64       `json:"budget_min_per_second"`
	BudgetBurst      float64       `json:"budget_burst"`
	_                time.Duration // keep time imported for the helpers below
}

type BreakerConfig struct {
	FailureThreshold uint32 `json:"failure_threshold"`
	MinimumRequests  uint32 `json:"minimum_requests"`
	OpenDurationMs   int64  `json:"open_duration_ms"`
	HalfOpenProbes   uint32 `json:"half_open_probes"`
	SuccessThreshold uint32 `json:"success_threshold"`
}

type ShedderConfig struct {
	MaxConcurrent uint32  `json:"max_concurrent"`
	MaxQueueDepth uint32  `json:"max_queue_depth"`
	SoftThreshold float64 `json:"soft_threshold"`
}

type RateLimitConfig struct {
	Enabled   bool    `json:"enabled"`
	PerSecond float64 `json:"per_second"`
	Burst     float64 `json:"burst"`
	MaxKeys   int     `json:"max_keys"`
}

type Route struct {
	Cluster    string       `json:"cluster"`
	PathPrefix string       `json:"path_prefix"`
	Balancer   BalancerKind `json:"balancer"`
	HashSource HashSource   `json:"hash_source"`
	HashHeader string       `json:"hash_header"`

	PerAttemptTimeoutMs int64 `json:"per_attempt_timeout_ms"`
	OverallDeadlineMs   int64 `json:"overall_deadline_ms"`

	Retry     RetryConfig     `json:"retry"`
	Breaker   BreakerConfig   `json:"breaker"`
	Shedder   ShedderConfig   `json:"shedder"`
	RateLimit RateLimitConfig `json:"rate_limit"`
}

func (r Route) Validate() error {
	if r.Cluster == "" {
		return errors.New("route has no cluster")
	}
	if !r.Balancer.Valid() {
		return fmt.Errorf("route %s has unknown balancer %q", r.PathPrefix, r.Balancer)
	}
	if r.Balancer == RingHash && r.HashSource == HashNone {
		// Ring hashing with no key hashes everything to zero and sends the
		// whole cluster's traffic to one endpoint. A config that cannot work
		// should be refused rather than published.
		return fmt.Errorf("route %s uses ring_hash with hash_source none", r.PathPrefix)
	}
	if r.HashSource == HashHeader && r.HashHeader == "" {
		return fmt.Errorf("route %s hashes on a header but names none", r.PathPrefix)
	}
	if r.OverallDeadlineMs > 0 && r.PerAttemptTimeoutMs > r.OverallDeadlineMs {
		// Not fatal in the data plane — it clamps — but it means the operator
		// wrote something they did not mean, and silently clamping a timeout
		// is how a config ends up not doing what its author believes.
		return fmt.Errorf("route %s: per-attempt timeout %dms exceeds overall deadline %dms",
			r.PathPrefix, r.PerAttemptTimeoutMs, r.OverallDeadlineMs)
	}
	if r.Breaker.OpenDurationMs < 0 {
		return fmt.Errorf("route %s has a negative breaker open duration", r.PathPrefix)
	}
	if r.Shedder.SoftThreshold < 0 || r.Shedder.SoftThreshold > 1 {
		return fmt.Errorf("route %s soft_threshold %.2f is outside [0,1]",
			r.PathPrefix, r.Shedder.SoftThreshold)
	}
	if r.Retry.BudgetRatio < 0 {
		return fmt.Errorf("route %s has a negative retry budget ratio", r.PathPrefix)
	}
	return nil
}

// Snapshot is a complete configuration, versioned.
//
// Whole snapshots rather than deltas, for the reason in the package comment:
// a delta needs both sides to agree about the state it applies to, and a lost
// message breaks that agreement silently and permanently.
type Snapshot struct {
	Version  uint64    `json:"version"`
	Clusters []Cluster `json:"clusters"`
	Routes   []Route   `json:"routes"`
}

// Validate checks a snapshot before it is published.
//
// Validation lives here, on the publishing side, because the data plane has
// no good option on receiving a bad config: refusing it leaves it running
// something stale that the operator believes they replaced, and accepting it
// breaks traffic. Rejecting at the control plane means the operator gets an
// error at the moment they made the mistake.
func (s Snapshot) Validate() error {
	if len(s.Clusters) == 0 {
		return errors.New("snapshot has no clusters")
	}
	if len(s.Routes) == 0 {
		return errors.New("snapshot has no routes")
	}

	known := map[string]Cluster{}
	for _, c := range s.Clusters {
		if err := c.Validate(); err != nil {
			return err
		}
		if _, dup := known[c.Name]; dup {
			return fmt.Errorf("cluster %s defined twice", c.Name)
		}
		known[c.Name] = c
	}

	prefixes := map[string]bool{}
	for _, r := range s.Routes {
		if err := r.Validate(); err != nil {
			return err
		}
		if _, ok := known[r.Cluster]; !ok {
			// A route to a nonexistent cluster is a 404 for every matching
			// request, and the data plane cannot tell that from a deliberate
			// catch-all.
			return fmt.Errorf("route %s names unknown cluster %s", r.PathPrefix, r.Cluster)
		}
		if prefixes[r.PathPrefix] {
			// Two routes with the same prefix means one is unreachable, and
			// which one depends on sort order — so the config does something
			// its author cannot predict.
			return fmt.Errorf("two routes share the prefix %q", r.PathPrefix)
		}
		prefixes[r.PathPrefix] = true
	}
	return nil
}

// Warnings are problems worth telling an operator about that are not grounds
// for refusing the config.
//
// The distinction matters. A cluster with no healthy endpoints is almost
// certainly a mistake, but it is also exactly what a snapshot looks like
// during a full redeploy — and refusing to publish during a redeploy would
// leave the data plane pinned to the pre-deploy endpoint list, which is worse.
func (s Snapshot) Warnings() []string {
	var out []string
	for _, c := range s.Clusters {
		if c.EligibleCount() == 0 {
			out = append(out, fmt.Sprintf(
				"cluster %s has no endpoints eligible for traffic: every request to it will be a 503",
				c.Name))
		}
		unknown := 0
		for _, e := range c.Endpoints {
			if e.Health == HealthUnknown {
				unknown++
			}
		}
		if unknown > 0 && unknown == len(c.Endpoints) {
			out = append(out, fmt.Sprintf(
				"cluster %s has %d endpoints and none have been health-checked yet",
				c.Name, unknown))
		}
	}

	referenced := map[string]bool{}
	for _, r := range s.Routes {
		referenced[r.Cluster] = true
	}
	for _, c := range s.Clusters {
		if !referenced[c.Name] {
			out = append(out, fmt.Sprintf("cluster %s is not referenced by any route", c.Name))
		}
	}

	hasCatchAll := false
	for _, r := range s.Routes {
		if r.PathPrefix == "" || r.PathPrefix == "/" {
			hasCatchAll = true
		}
	}
	if !hasCatchAll {
		out = append(out, "no catch-all route: unmatched requests will get 404")
	}

	sort.Strings(out)
	return out
}

// Normalize fills in defaults and canonicalises the snapshot.
//
// Run before validation so an operator writing minimal config gets working
// defaults rather than a wall of errors. Every default here is the
// conservative choice: short timeouts, few attempts, a tight retry budget.
// A proxy whose defaults are generous is one that amplifies its first outage.
func (s *Snapshot) Normalize() {
	for i := range s.Clusters {
		for j := range s.Clusters[i].Endpoints {
			e := &s.Clusters[i].Endpoints[j]
			if e.Weight == 0 && e.Health == "" {
				// Weight zero with no stated health is almost certainly an
				// omission rather than a deliberate drain, so default it to 1.
				// An explicit zero weight alongside an explicit health is
				// respected as a drain.
				e.Weight = 1
			}
			if e.Health == "" {
				e.Health = HealthUnknown
			}
			e.Host = strings.TrimSpace(e.Host)
		}
		sort.Slice(s.Clusters[i].Endpoints, func(a, b int) bool {
			return s.Clusters[i].Endpoints[a].Address() <
				s.Clusters[i].Endpoints[b].Address()
		})
	}

	for i := range s.Routes {
		r := &s.Routes[i]
		if r.Balancer == "" {
			r.Balancer = P2C
		}
		if r.HashSource == "" {
			if r.Balancer == RingHash {
				r.HashSource = HashPath
			} else {
				r.HashSource = HashNone
			}
		}
		if r.PerAttemptTimeoutMs == 0 {
			r.PerAttemptTimeoutMs = 1000
		}
		if r.OverallDeadlineMs == 0 {
			r.OverallDeadlineMs = 3000
		}
		if r.Retry.MaxAttempts == 0 {
			r.Retry.MaxAttempts = 3
		}
		if r.Retry.InitialBackoffMs == 0 {
			r.Retry.InitialBackoffMs = 10
		}
		if r.Retry.MaxBackoffMs == 0 {
			r.Retry.MaxBackoffMs = 500
		}
		if r.Retry.Multiplier == 0 {
			r.Retry.Multiplier = 2
		}
		if r.Retry.Jitter == 0 {
			// Jitter is not optional, so a zero here is treated as unset
			// rather than as "no jitter". Without it every client that failed
			// together retries together. An operator who genuinely wants no
			// jitter has to be explicit in a way this field cannot express,
			// which is a deliberate trade: the failure mode of accidental
			// zero jitter is much worse than the inconvenience.
			r.Retry.Jitter = 0.5
		}
		if r.Retry.BudgetRatio == 0 {
			r.Retry.BudgetRatio = 0.2
		}
		if r.Retry.BudgetMinPerSec == 0 {
			r.Retry.BudgetMinPerSec = 3
		}
		if r.Retry.BudgetBurst == 0 {
			r.Retry.BudgetBurst = 20
		}
		if r.Breaker.FailureThreshold == 0 {
			r.Breaker.FailureThreshold = 5
		}
		if r.Breaker.MinimumRequests == 0 {
			r.Breaker.MinimumRequests = 10
		}
		if r.Breaker.OpenDurationMs == 0 {
			r.Breaker.OpenDurationMs = 5000
		}
		if r.Breaker.HalfOpenProbes == 0 {
			r.Breaker.HalfOpenProbes = 3
		}
		if r.Breaker.SuccessThreshold == 0 {
			r.Breaker.SuccessThreshold = 2
		}
		if r.Shedder.MaxConcurrent == 0 {
			r.Shedder.MaxConcurrent = 1024
		}
		if r.Shedder.MaxQueueDepth == 0 {
			r.Shedder.MaxQueueDepth = 256
		}
		if r.Shedder.SoftThreshold == 0 {
			r.Shedder.SoftThreshold = 0.5
		}
		if r.RateLimit.Enabled {
			if r.RateLimit.PerSecond == 0 {
				r.RateLimit.PerSecond = 1000
			}
			if r.RateLimit.Burst == 0 {
				r.RateLimit.Burst = r.RateLimit.PerSecond * 2
			}
			if r.RateLimit.MaxKeys == 0 {
				r.RateLimit.MaxKeys = 65536
			}
		}
	}

	// Longest prefix first, matching the data plane's own ordering so the
	// published snapshot reads the way it will behave.
	sort.Slice(s.Routes, func(a, b int) bool {
		if len(s.Routes[a].PathPrefix) != len(s.Routes[b].PathPrefix) {
			return len(s.Routes[a].PathPrefix) > len(s.Routes[b].PathPrefix)
		}
		return s.Routes[a].PathPrefix < s.Routes[b].PathPrefix
	})
}

// Fnv1a64 mirrors the data plane's hash exactly, so the control plane can
// predict which endpoint a key will land on.
//
// FNV-1a followed by the MurmurHash3 64-bit finalizer. The finalizer is not
// optional: plain FNV-1a diffuses poorly into the high bits for short keys
// sharing a prefix, and ring points are exactly that — so without it every
// one of an endpoint's virtual nodes lands in one narrow arc and the ring
// degenerates into a handful of giant arcs. The C++ side has the same comment
// because the two must not drift.
func Fnv1a64(data string) uint64 {
	var hash uint64 = 0xcbf29ce484222325
	for i := 0; i < len(data); i++ {
		hash ^= uint64(data[i])
		hash *= 0x100000001b3
	}
	hash ^= hash >> 33
	hash *= 0xff51afd7ed558ccd
	hash ^= hash >> 33
	hash *= 0xc4ceb9fe1a85ec53
	hash ^= hash >> 33
	return hash
}
