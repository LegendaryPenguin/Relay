package api

import (
	"sort"
	"strconv"

	"github.com/LegendaryPenguin/relay/config"
)

// Ring mirrors the data plane's consistent hash ring.
//
// A second implementation of the same structure, in a different language, by
// the same author. The honest statement of what that buys: it catches
// transcription errors and lets the control plane answer "where would this key
// go" without tailing proxy logs. It does not catch a misreading of the
// algorithm, because there is only one reading here. The cross-check that
// matters is the pinned hash vector, which both sides assert.
type Ring struct {
	points   []ringPoint
	endpoint []int
}

type ringPoint struct {
	hash  uint64
	index int
}

// BuildRing constructs the ring for a cluster's eligible endpoints.
//
// Must match the C++ side exactly: same key format ("host:port#replica"), same
// hash, same weight scaling. A divergence would mean the control plane's
// placement answers are confidently wrong, which is worse than not having the
// endpoint at all.
func BuildRing(c config.Cluster, virtualNodes int) *Ring {
	if virtualNodes <= 0 {
		virtualNodes = 160
	}
	r := &Ring{}
	for i, e := range c.Endpoints {
		if e.Health != config.HealthHealthy || e.Weight == 0 {
			continue
		}
		points := virtualNodes * int(e.Weight)
		base := e.Address()
		for v := 0; v < points; v++ {
			r.points = append(r.points, ringPoint{
				hash:  config.Fnv1a64(base + "#" + strconv.Itoa(v)),
				index: i,
			})
		}
	}
	sort.Slice(r.points, func(a, b int) bool { return r.points[a].hash < r.points[b].hash })

	// Deduplicate, matching the C++ side. Collisions are vanishingly unlikely
	// at 64 bits, but leaving them in makes the lookup result depend on sort
	// stability, which is a source of divergence between the two
	// implementations for no benefit.
	if len(r.points) > 0 {
		out := r.points[:1]
		for _, p := range r.points[1:] {
			if p.hash != out[len(out)-1].hash {
				out = append(out, p)
			}
		}
		r.points = out
	}
	return r
}

func (r *Ring) Size() int { return len(r.points) }

// Lookup returns the endpoint index owning a key.
func (r *Ring) Lookup(keyHash uint64) (int, bool) {
	if len(r.points) == 0 {
		return 0, false
	}
	i := sort.Search(len(r.points), func(i int) bool { return r.points[i].hash >= keyHash })
	if i == len(r.points) {
		i = 0
	}
	return r.points[i].index, true
}
