#include "relay/upstream.hpp"

#include <algorithm>
#include <cassert>

namespace relay {

const char* HealthName(Health h) {
    switch (h) {
        case Health::kUnknown: return "unknown";
        case Health::kHealthy: return "healthy";
        case Health::kUnhealthy: return "unhealthy";
    }
    return "invalid";
}

const char* BalancerKindName(BalancerKind k) {
    switch (k) {
        case BalancerKind::kRoundRobin: return "round_robin";
        case BalancerKind::kLeastRequest: return "least_request";
        case BalancerKind::kP2C: return "p2c";
        case BalancerKind::kRandom: return "random";
        case BalancerKind::kRingHash: return "ring_hash";
    }
    return "invalid";
}

std::string Endpoint::Address() const {
    return host + ":" + std::to_string(port);
}

ClusterSnapshot::ClusterSnapshot(std::string name, std::vector<Endpoint> endpoints)
    : name_(std::move(name)), endpoints_(std::move(endpoints)) {
    // The eligible index is computed once at construction rather than filtered
    // per request. Snapshots are published a few times an hour and read
    // millions of times, so anything derivable belongs here.
    eligible_.reserve(endpoints_.size());
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        if (endpoints_[i].eligible()) eligible_.push_back(i);
    }
}

uint64_t Fnv1a64(std::string_view data) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    // Avalanche. This is not decoration — without it the ring is broken.
    //
    // Plain FNV-1a mixes well into the *low* bits and poorly into the high
    // ones: the final byte is XORed and multiplied just once, so for short
    // keys sharing a prefix the top bits stay dominated by that prefix. Ring
    // points are exactly such keys ("10.0.0.1:8080#0", "#1", ...), and the
    // ring is sorted by the full 64-bit value, so every one of an endpoint's
    // 160 points landed in one narrow arc. A 10-endpoint ring became 10 giant
    // arcs instead of 1600 interleaved ones, and the measured worst-case
    // share deviation was 99% rather than the ~15% the structure should give.
    //
    // The MurmurHash3 64-bit finalizer fixes it by diffusing every input bit
    // across the whole word. The Go control plane implements the identical
    // function so it can predict placement, so this definition is a
    // cross-language contract and not an implementation detail.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    hash ^= hash >> 33;
    return hash;
}

// ---------------------------------------------------------------------------
// HashRing
// ---------------------------------------------------------------------------

HashRing::HashRing(const ClusterSnapshot& snapshot, size_t virtual_nodes) {
    const auto& eligible = snapshot.eligible();
    if (eligible.empty() || virtual_nodes == 0) return;

    ring_.reserve(eligible.size() * virtual_nodes);
    for (size_t idx : eligible) {
        const Endpoint& ep = snapshot.endpoints()[idx];
        // Virtual node count scales with weight, so weight means the same
        // thing here as in the other balancers. Without this, a weighted
        // endpoint would be honoured by round-robin and ignored by the ring,
        // and a config change that only switched the balancer would silently
        // change traffic distribution.
        size_t points = virtual_nodes * ep.weight;
        std::string base = ep.Address();
        for (size_t v = 0; v < points; ++v) {
            // The point is hashed from address plus replica index. Deriving it
            // from the *address* rather than the endpoint's position in the
            // list is what makes the ring stable: adding an endpoint shifts
            // every later index but changes nobody's hash.
            std::string key = base;
            key += '#';
            key += std::to_string(v);
            ring_.push_back({Fnv1a64(key), idx});
        }
    }

    std::sort(ring_.begin(), ring_.end(),
              [](const Node& a, const Node& b) { return a.point < b.point; });

    // Duplicate points would make Lookup's result depend on sort stability.
    // Collisions are vanishingly unlikely at 64 bits but the dedup is cheap
    // and removes a source of nondeterminism between processes.
    ring_.erase(std::unique(ring_.begin(), ring_.end(),
                            [](const Node& a, const Node& b) { return a.point == b.point; }),
                ring_.end());
}

std::optional<size_t> HashRing::Lookup(uint64_t key_hash) const {
    if (ring_.empty()) return std::nullopt;
    // First point >= key, wrapping to the start. This is the definition of
    // the ring and the reason removal only affects the removed endpoint's
    // keys: every other key still finds the same successor.
    auto it = std::lower_bound(ring_.begin(), ring_.end(), key_hash,
                               [](const Node& n, uint64_t k) { return n.point < k; });
    if (it == ring_.end()) it = ring_.begin();
    return it->endpoint_index;
}

std::vector<size_t> HashRing::LookupChain(uint64_t key_hash, size_t max) const {
    std::vector<size_t> out;
    if (ring_.empty() || max == 0) return out;

    auto start = std::lower_bound(ring_.begin(), ring_.end(), key_hash,
                                  [](const Node& n, uint64_t k) { return n.point < k; });
    if (start == ring_.end()) start = ring_.begin();

    // Walk forward collecting distinct endpoints. Deterministic order matters:
    // two proxies failing over to different replacements would defeat the
    // affinity the ring exists to provide, so the chain is a property of the
    // key and the ring rather than of local state.
    for (size_t step = 0; step < ring_.size() && out.size() < max; ++step) {
        auto it = start + static_cast<long>(step);
        if (it >= ring_.end()) it -= static_cast<long>(ring_.size());
        size_t idx = it->endpoint_index;
        if (std::find(out.begin(), out.end(), idx) == out.end()) out.push_back(idx);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Balancer
// ---------------------------------------------------------------------------

Balancer::Balancer(BalancerKind kind, uint64_t seed) : kind_(kind), rng_state_(seed) {
    // A zero seed would leave xorshift stuck at zero forever, which presents
    // as every random pick landing on endpoint 0.
    if (rng_state_ == 0) rng_state_ = 0x9e3779b97f4a7c15ull;
}

void Balancer::SetSnapshot(ClusterSnapshotPtr snapshot) {
    snapshot_ = std::move(snapshot);
    if (!snapshot_) {
        ring_ = HashRing();
        load_.clear();
        return;
    }
    if (kind_ == BalancerKind::kRingHash) {
        ring_ = HashRing(*snapshot_);
    }
    // Live load counts are resized, not cleared. A config push that dropped
    // the in-flight counts would make every endpoint look idle and the next
    // few hundred requests would pile onto whichever one sorted first. Growing
    // preserves the existing prefix, which is correct as long as the control
    // plane appends rather than reorders — and that assumption is stated in
    // the README as a real constraint.
    load_.resize(snapshot_->endpoints().size());
    rr_cursor_ = 0;
}

uint64_t Balancer::NextRandom() {
    // xorshift64*. Fast, adequate for load balancing, and deterministic from
    // the seed so the distribution tests reproduce exactly.
    rng_state_ ^= rng_state_ >> 12;
    rng_state_ ^= rng_state_ << 25;
    rng_state_ ^= rng_state_ >> 27;
    return rng_state_ * 0x2545f4914f6cdd1dull;
}

std::optional<size_t> Balancer::Pick(uint64_t key_hash) {
    if (!snapshot_) return std::nullopt;
    const auto& eligible = snapshot_->eligible();
    if (eligible.empty()) return std::nullopt;

    switch (kind_) {
        case BalancerKind::kRoundRobin: return PickRoundRobin(eligible);
        case BalancerKind::kLeastRequest: return PickLeastRequest(eligible);
        case BalancerKind::kP2C: return PickP2C(eligible);
        case BalancerKind::kRandom: return PickRandom(eligible);
        case BalancerKind::kRingHash: return PickRing(key_hash);
    }
    return std::nullopt;
}

size_t Balancer::PickRoundRobin(const std::vector<size_t>& eligible) {
    // Weighted round robin, expanded on the fly rather than by materialising a
    // weighted list. A materialised list of 1000 entries for a weight-1000
    // endpoint is memory the snapshot would have to carry.
    size_t total_weight = 0;
    for (size_t idx : eligible) total_weight += snapshot_->endpoints()[idx].weight;
    if (total_weight == 0) return eligible.front();

    size_t target = rr_cursor_ % total_weight;
    ++rr_cursor_;
    size_t running = 0;
    for (size_t idx : eligible) {
        running += snapshot_->endpoints()[idx].weight;
        if (target < running) return idx;
    }
    return eligible.back();
}

size_t Balancer::PickLeastRequest(const std::vector<size_t>& eligible) {
    // A full scan. O(n) per request, which is the cost that motivates P2C and
    // is measured in bench/results.md.
    size_t best = eligible.front();
    // Weight-adjusted: an endpoint with twice the weight should carry twice
    // the load before it looks equally busy. Comparing a/w via cross
    // multiplication avoids a division and the integer-truncation ties it
    // would create.
    for (size_t idx : eligible) {
        const auto& a = load_[idx];
        const auto& b = load_[best];
        uint64_t wa = snapshot_->endpoints()[idx].weight;
        uint64_t wb = snapshot_->endpoints()[best].weight;
        if (a.active * wb < b.active * wa) best = idx;
    }
    return best;
}

size_t Balancer::PickP2C(const std::vector<size_t>& eligible) {
    if (eligible.size() == 1) return eligible.front();

    // Two *distinct* choices, by drawing the second from a range one smaller
    // and skipping over the first. Drawing twice independently and retrying on
    // a collision would also work but biases nothing and costs a loop; this is
    // exact and branch-free. Without distinctness the degenerate same-pick
    // case silently degrades to plain random, which is measurably worse and
    // invisible in aggregate numbers.
    size_t n = eligible.size();
    size_t pi = NextRandom() % n;
    size_t pj = NextRandom() % (n - 1);
    if (pj >= pi) ++pj;
    size_t i = eligible[pi];
    size_t j = eligible[pj];

    uint64_t wi = snapshot_->endpoints()[i].weight;
    uint64_t wj = snapshot_->endpoints()[j].weight;
    return load_[i].active * wj <= load_[j].active * wi ? i : j;
}

size_t Balancer::PickRandom(const std::vector<size_t>& eligible) {
    return eligible[NextRandom() % eligible.size()];
}

std::optional<size_t> Balancer::PickRing(uint64_t key_hash) {
    if (ring_.empty()) return std::nullopt;
    // The chain, not just the owner. The owner may have gone unhealthy since
    // the ring was built — the ring is built from eligible endpoints, but the
    // snapshot can be a moment stale relative to health probes, and failing
    // the request when a deterministic fallback exists is worse than a small
    // affinity loss.
    auto chain = ring_.LookupChain(key_hash, 8);
    for (size_t idx : chain) {
        if (snapshot_->endpoints()[idx].eligible()) return idx;
    }
    return std::nullopt;
}

void Balancer::OnRequestStart(size_t endpoint_index) {
    if (endpoint_index >= load_.size()) return;
    ++load_[endpoint_index].active;
    ++load_[endpoint_index].total;
}

void Balancer::OnRequestEnd(size_t endpoint_index, bool failed) {
    if (endpoint_index >= load_.size()) return;
    auto& l = load_[endpoint_index];
    // Guarded against underflow rather than asserted. A missed OnRequestStart
    // — from an early return on a path added later — would otherwise wrap the
    // counter to 2^64 and make the endpoint look infinitely loaded, so
    // least-request would never choose it again. Silently never routing to a
    // healthy endpoint is a much worse failure than an off-by-one count.
    if (l.active > 0) --l.active;
    if (failed) ++l.failures;
}

const EndpointLoad& Balancer::load(size_t endpoint_index) const {
    static const EndpointLoad kEmpty{};
    if (endpoint_index >= load_.size()) return kEmpty;
    return load_[endpoint_index];
}

}  // namespace relay
