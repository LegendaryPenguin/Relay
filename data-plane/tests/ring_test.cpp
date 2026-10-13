#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "relay/upstream.hpp"
#include "testing.hpp"

using namespace relay;

namespace {

ClusterSnapshotPtr MakeCluster(int count, uint32_t weight = 1) {
    std::vector<Endpoint> eps;
    for (int i = 0; i < count; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = weight;
        ep.health = Health::kHealthy;
        eps.push_back(ep);
    }
    return std::make_shared<ClusterSnapshot>("c", std::move(eps));
}

std::vector<uint64_t> Keys(size_t n) {
    std::vector<uint64_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(Fnv1a64("key-" + std::to_string(i)));
    return out;
}

}  // namespace

RELAY_TEST(empty_ring_returns_nothing) {
    auto cluster = std::make_shared<ClusterSnapshot>("c", std::vector<Endpoint>{});
    HashRing ring(*cluster);
    CHECK(ring.empty());
    CHECK(!ring.Lookup(12345).has_value());
}

RELAY_TEST(lookup_is_deterministic) {
    auto cluster = MakeCluster(5);
    HashRing ring(*cluster);
    for (uint64_t key : Keys(200)) {
        auto first = ring.Lookup(key);
        CHECK(first.has_value());
        for (int i = 0; i < 5; ++i) CHECK_EQ(ring.Lookup(key), first);
    }
}

RELAY_TEST(two_rings_from_the_same_membership_agree) {
    // Ring points are derived from endpoint addresses, not list positions, so
    // two independently built rings over the same membership must be
    // identical. Without that, two proxies would route the same key
    // differently and the affinity would be worthless.
    auto a = MakeCluster(7);
    auto b = MakeCluster(7);
    HashRing ring_a(*a);
    HashRing ring_b(*b);
    for (uint64_t key : Keys(500)) CHECK_EQ(ring_a.Lookup(key), ring_b.Lookup(key));
}

RELAY_TEST(distribution_is_even_with_virtual_nodes) {
    // A ring with one point per endpoint is wildly uneven. 160 virtual nodes
    // should keep every endpoint within a reasonable band of its fair share.
    const int kEndpoints = 10;
    auto cluster = MakeCluster(kEndpoints);
    HashRing ring(*cluster, HashRing::kDefaultVirtualNodes);

    std::map<size_t, int> counts;
    const int kKeys = 100000;
    for (int i = 0; i < kKeys; ++i) {
        auto idx = ring.Lookup(Fnv1a64("obj-" + std::to_string(i)));
        CHECK(idx.has_value());
        counts[*idx]++;
    }

    CHECK_EQ(counts.size(), size_t(kEndpoints));
    double fair = static_cast<double>(kKeys) / kEndpoints;
    double worst = 0;
    for (const auto& [idx, n] : counts) {
        worst = std::max(worst, std::abs(n - fair) / fair);
    }
    // 25% is loose but honest: consistent hashing with 160 vnodes over 10
    // endpoints really does deviate by this much, and asserting 5% would make
    // the test fail for reasons that are not bugs. bench/results.md reports
    // the measured spread at several vnode counts.
    if (worst > 0.25) {
        std::fprintf(stderr, "        worst deviation %.1f%%\n", worst * 100);
        CHECK(false);
    }
}

RELAY_TEST(few_virtual_nodes_is_measurably_worse) {
    // The justification for paying 160 points per endpoint. If one point per
    // endpoint were fine, the virtual nodes would be waste.
    const int kEndpoints = 10;
    const int kKeys = 50000;
    auto cluster = MakeCluster(kEndpoints);

    auto spread = [&](size_t vnodes) {
        HashRing ring(*cluster, vnodes);
        std::map<size_t, int> counts;
        for (int i = 0; i < kKeys; ++i) {
            auto idx = ring.Lookup(Fnv1a64("obj-" + std::to_string(i)));
            if (idx.has_value()) counts[*idx]++;
        }
        double fair = static_cast<double>(kKeys) / kEndpoints;
        double worst = 0;
        for (const auto& [idx, n] : counts) worst = std::max(worst, std::abs(n - fair) / fair);
        return worst;
    };

    double few = spread(1);
    double many = spread(160);
    if (!(few > many)) {
        std::fprintf(stderr, "        1 vnode: %.1f%%, 160 vnodes: %.1f%%\n", few * 100,
                     many * 100);
        CHECK(false);
    }
}

RELAY_TEST(removal_remaps_only_the_removed_share) {
    // The property the whole data structure exists for. Removing one of ten
    // endpoints should move roughly a tenth of keys and leave the rest
    // exactly where they were. hash(key) % n remaps nearly everything.
    const int kEndpoints = 10;
    auto before = MakeCluster(kEndpoints);
    HashRing ring_before(*before);

    // Drop the last endpoint.
    std::vector<Endpoint> fewer = before->endpoints();
    fewer.pop_back();
    auto after = std::make_shared<ClusterSnapshot>("c", fewer);
    HashRing ring_after(*after);

    const int kKeys = 50000;
    int moved = 0;
    for (int i = 0; i < kKeys; ++i) {
        uint64_t key = Fnv1a64("obj-" + std::to_string(i));
        auto a = ring_before.Lookup(key);
        auto b = ring_after.Lookup(key);
        CHECK(a.has_value() && b.has_value());
        if (*a != *b) ++moved;
    }

    double fraction = static_cast<double>(moved) / kKeys;
    // Expect about 1/10. Allow a generous band; the point is that it is
    // nowhere near 1.0.
    if (fraction > 0.20) {
        std::fprintf(stderr, "        removal moved %.1f%% of keys\n", fraction * 100);
        CHECK(false);
    }
    // And it must move *something*, or the test would pass on a broken ring
    // that ignored membership.
    if (fraction < 0.03) {
        std::fprintf(stderr, "        removal moved only %.2f%% of keys\n", fraction * 100);
        CHECK(false);
    }
}

RELAY_TEST(addition_remaps_only_the_new_share) {
    const int kEndpoints = 10;
    auto before = MakeCluster(kEndpoints);
    HashRing ring_before(*before);

    std::vector<Endpoint> more = before->endpoints();
    Endpoint extra;
    extra.host = "10.0.0.99";
    extra.port = 8080;
    extra.weight = 1;
    extra.health = Health::kHealthy;
    more.push_back(extra);
    auto after = std::make_shared<ClusterSnapshot>("c", more);
    HashRing ring_after(*after);

    const int kKeys = 50000;
    int moved = 0;
    for (int i = 0; i < kKeys; ++i) {
        uint64_t key = Fnv1a64("obj-" + std::to_string(i));
        if (ring_before.Lookup(key) != ring_after.Lookup(key)) ++moved;
    }
    double fraction = static_cast<double>(moved) / kKeys;
    // Adding the 11th endpoint should claim about 1/11 of keys.
    if (fraction > 0.20 || fraction < 0.03) {
        std::fprintf(stderr, "        addition moved %.1f%% of keys\n", fraction * 100);
        CHECK(false);
    }
}

RELAY_TEST(weight_scales_ring_share) {
    // Weight must mean the same thing for the ring as for the other
    // balancers, or switching balancer in config would silently change
    // traffic distribution.
    std::vector<Endpoint> eps;
    for (int i = 0; i < 3; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = (i == 0) ? 4u : 1u;
        ep.health = Health::kHealthy;
        eps.push_back(ep);
    }
    auto cluster = std::make_shared<ClusterSnapshot>("c", eps);
    HashRing ring(*cluster);

    std::map<size_t, int> counts;
    const int kKeys = 60000;
    for (int i = 0; i < kKeys; ++i) {
        auto idx = ring.Lookup(Fnv1a64("obj-" + std::to_string(i)));
        if (idx.has_value()) counts[*idx]++;
    }
    // Endpoint 0 has weight 4 of total 6, so should get roughly 2/3.
    double share0 = static_cast<double>(counts[0]) / kKeys;
    if (share0 < 0.55 || share0 > 0.78) {
        std::fprintf(stderr, "        weighted endpoint got %.1f%%, expected ~66%%\n",
                     share0 * 100);
        CHECK(false);
    }
}

RELAY_TEST(lookup_chain_is_deterministic_and_distinct) {
    // Failover must be a property of the key and the ring, not of local
    // state: two proxies failing over to different replacements defeats the
    // affinity the ring provides.
    auto cluster = MakeCluster(6);
    HashRing ring(*cluster);
    for (uint64_t key : Keys(100)) {
        auto chain = ring.LookupChain(key, 4);
        CHECK(!chain.empty());
        std::set<size_t> distinct(chain.begin(), chain.end());
        CHECK_EQ(distinct.size(), chain.size());
        // The head of the chain is the natural owner.
        CHECK_EQ(chain[0], *ring.Lookup(key));
        // Repeated calls agree.
        CHECK(ring.LookupChain(key, 4) == chain);
    }
}

RELAY_TEST(lookup_chain_bounded_by_endpoint_count) {
    auto cluster = MakeCluster(3);
    HashRing ring(*cluster);
    auto chain = ring.LookupChain(Fnv1a64("k"), 10);
    CHECK_EQ(chain.size(), size_t(3));
}

RELAY_TEST(unhealthy_endpoints_are_not_in_the_ring) {
    std::vector<Endpoint> eps;
    for (int i = 0; i < 4; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = 1;
        ep.health = (i == 1) ? Health::kUnhealthy : Health::kHealthy;
        eps.push_back(ep);
    }
    auto cluster = std::make_shared<ClusterSnapshot>("c", eps);
    HashRing ring(*cluster);
    for (uint64_t key : Keys(500)) {
        auto idx = ring.Lookup(key);
        CHECK(idx.has_value());
        CHECK(*idx != 1);
    }
}

RELAY_TEST(drained_endpoint_excluded) {
    // Weight zero means drain: keeps existing connections, takes no new work.
    std::vector<Endpoint> eps;
    for (int i = 0; i < 3; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = (i == 0) ? 0u : 1u;
        ep.health = Health::kHealthy;
        eps.push_back(ep);
    }
    auto cluster = std::make_shared<ClusterSnapshot>("c", eps);
    CHECK_EQ(cluster->eligible_count(), size_t(2));
    HashRing ring(*cluster);
    for (uint64_t key : Keys(300)) {
        auto idx = ring.Lookup(key);
        CHECK(idx.has_value());
        CHECK(*idx != 0);
    }
}

RELAY_TEST(hash_is_stable_across_calls) {
    // The Go control plane computes the same hash to predict placement, so a
    // change here is a cross-language protocol break. The value is FNV-1a
    // followed by the MurmurHash3 finalizer, not raw FNV-1a — the finalizer
    // is load-bearing, see the comment in Fnv1a64.
    CHECK_EQ(Fnv1a64(""), 17280346270528514342ull);
    CHECK_EQ(Fnv1a64("a"), Fnv1a64("a"));
    CHECK(Fnv1a64("a") != Fnv1a64("b"));
    // Raw FNV-1a's offset basis must NOT come back, or the finalizer was
    // dropped and the ring silently degrades to ten giant arcs.
    CHECK(Fnv1a64("") != 0xcbf29ce484222325ull);
}

RELAY_TEST_MAIN()
