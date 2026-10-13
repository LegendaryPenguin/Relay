#pragma once

// Endpoints, load balancing, and the consistent hash ring.
//
// Selection is deliberately separated from proxying. The balancer takes a
// snapshot of endpoints and a request hash and returns an endpoint; it opens
// no sockets and knows nothing about HTTP. That split is what made it possible
// to test the distribution properties exhaustively — the tests in
// tests/balancer_test.cpp run a million selections, which would be impossible
// if choosing an endpoint required a connection.

#include <cstdint>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <vector>

#include "relay/status.hpp"

namespace relay {

// Health, as the data plane understands it.
//
// Three states rather than a boolean, for the same reason Dovetail's worker
// registry has three: a newly added endpoint has not been probed yet, and
// treating "unknown" as either healthy or unhealthy is wrong. Healthy means
// traffic goes there on faith; unhealthy means a freshly deployed endpoint
// never receives traffic and so never gets probed by real requests.
enum class Health {
    kUnknown,
    kHealthy,
    kUnhealthy,
};

const char* HealthName(Health h);

// Printable so assertions and log lines name the value rather than its
// integer. Found by ADL in namespace relay.
inline std::ostream& operator<<(std::ostream& os, Health h) { return os << HealthName(h); }

struct Endpoint {
    std::string host;
    uint16_t port = 0;
    // Weight scales how much traffic this endpoint receives. Zero means
    // drain: it keeps existing connections and receives no new requests.
    uint32_t weight = 1;
    Health health = Health::kUnknown;
    std::string zone;

    std::string Address() const;
    bool eligible() const { return health == Health::kHealthy && weight > 0; }
};

// A point-in-time, immutable set of endpoints for one cluster.
//
// Immutable and shared by `shared_ptr`. The control plane publishes a whole new
// snapshot rather than mutating the live one, and the proxy swaps the pointer
// atomically. That is the entire concurrency story for configuration: a
// request that started under the old snapshot finishes under it, and no reader
// ever holds a lock. A mutable endpoint list protected by a mutex was my first
// design and it put a lock acquisition on the hot path of every request for
// data that changes a few times an hour.
class ClusterSnapshot {
  public:
    ClusterSnapshot() = default;
    ClusterSnapshot(std::string name, std::vector<Endpoint> endpoints);

    const std::string& name() const { return name_; }
    const std::vector<Endpoint>& endpoints() const { return endpoints_; }

    // Indices into endpoints() that are eligible for new traffic.
    const std::vector<size_t>& eligible() const { return eligible_; }
    size_t eligible_count() const { return eligible_.size(); }

    uint64_t version() const { return version_; }
    void set_version(uint64_t v) { version_ = v; }

  private:
    std::string name_;
    std::vector<Endpoint> endpoints_;
    std::vector<size_t> eligible_;
    uint64_t version_ = 0;
};

using ClusterSnapshotPtr = std::shared_ptr<const ClusterSnapshot>;

// Per-endpoint live state the balancer reads.
//
// Separate from Endpoint because Endpoint comes from configuration and this is
// measured locally. Mixing them would mean a config push either clobbered the
// live request counts or had to merge them, and the merge is where the
// off-by-one that loses a decrement lives.
struct EndpointLoad {
    // Requests currently in flight. The quantity least-request balancing
    // minimises.
    uint64_t active = 0;
    uint64_t total = 0;
    uint64_t failures = 0;
};

// How to choose among eligible endpoints.
enum class BalancerKind {
    kRoundRobin,
    kLeastRequest,
    // Power of two choices: pick two at random, take the less loaded.
    //
    // The interesting one. Plain least-request needs a scan and, with
    // multiple proxies, suffers a herd problem where every proxy sends to the
    // same momentarily-idle endpoint. P2C is O(1) and provably close to
    // optimal; bench/results.md measures both.
    kP2C,
    kRandom,
    // Consistent hashing for affinity. Requires a request hash.
    kRingHash,
};

const char* BalancerKindName(BalancerKind k);

inline std::ostream& operator<<(std::ostream& os, BalancerKind k) {
    return os << BalancerKindName(k);
}

// Consistent hash ring with virtual nodes.
//
// Built from a snapshot, rebuilt when the snapshot changes. The property worth
// the implementation is *minimal disruption*: removing one endpoint of N
// should remap roughly 1/N of keys and leave the rest where they were. A naive
// `hash(key) % count` remaps nearly everything on any membership change, which
// for a cache fleet means a full cache miss storm on every deploy.
//
// Virtual nodes exist because a ring with one point per endpoint is wildly
// uneven — with 5 endpoints the largest share can be several times the
// smallest. The count is a direct quality/memory tradeoff and
// bench/results.md measures the distribution at several values.
class HashRing {
  public:
    static constexpr size_t kDefaultVirtualNodes = 160;

    HashRing() = default;
    HashRing(const ClusterSnapshot& snapshot, size_t virtual_nodes = kDefaultVirtualNodes);

    bool empty() const { return ring_.empty(); }
    size_t size() const { return ring_.size(); }

    // The endpoint index for a key, or nullopt if the ring is empty.
    std::optional<size_t> Lookup(uint64_t key_hash) const;

    // Walks forward from the key's position, yielding distinct endpoint
    // indices. Used to find a healthy endpoint when the natural owner is
    // unhealthy, which must be deterministic: two proxies failing over to
    // different replacements defeats the affinity the ring exists to provide.
    std::vector<size_t> LookupChain(uint64_t key_hash, size_t max) const;

  private:
    struct Node {
        uint64_t point;
        size_t endpoint_index;
    };
    std::vector<Node> ring_;  // Sorted by point.
};

// 64-bit FNV-1a. Used for ring points and request hashing.
//
// Not cryptographic and does not need to be: an attacker who can choose keys
// can concentrate load on one endpoint, but they could do that with any
// publicly known hash, and the defence is rate limiting rather than a
// different hash. Chosen for being short, dependency-free, and identical
// across the C++ and Go sides so the control plane can predict placement.
uint64_t Fnv1a64(std::string_view data);

// Chooses endpoints from a snapshot.
//
// Not thread-safe for mutation; the proxy holds one per worker thread. That is
// deliberate — a shared balancer needs atomics on the round-robin counter and
// the active counts, and per-thread instances make the hot path lock-free at
// the cost of slightly less even global distribution. The cost is measured in
// bench/results.md.
class Balancer {
  public:
    explicit Balancer(BalancerKind kind, uint64_t seed = 0x9e3779b97f4a7c15ull);

    // Installs a new snapshot, rebuilding the ring if needed.
    void SetSnapshot(ClusterSnapshotPtr snapshot);
    const ClusterSnapshotPtr& snapshot() const { return snapshot_; }

    // Picks an endpoint. `key_hash` is used only by kRingHash.
    //
    // Returns an index into snapshot()->endpoints(), or nullopt when nothing
    // is eligible — which the caller must map to 503 rather than retrying,
    // since a retry would find the same empty set.
    std::optional<size_t> Pick(uint64_t key_hash = 0);

    // Load accounting. Called around each attempt.
    void OnRequestStart(size_t endpoint_index);
    void OnRequestEnd(size_t endpoint_index, bool failed);

    const EndpointLoad& load(size_t endpoint_index) const;
    BalancerKind kind() const { return kind_; }

  private:
    size_t PickRoundRobin(const std::vector<size_t>& eligible);
    size_t PickLeastRequest(const std::vector<size_t>& eligible);
    size_t PickP2C(const std::vector<size_t>& eligible);
    size_t PickRandom(const std::vector<size_t>& eligible);
    std::optional<size_t> PickRing(uint64_t key_hash);

    uint64_t NextRandom();

    BalancerKind kind_;
    ClusterSnapshotPtr snapshot_;
    HashRing ring_;
    std::vector<EndpointLoad> load_;
    size_t rr_cursor_ = 0;
    uint64_t rng_state_;
};

}  // namespace relay
