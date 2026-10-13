#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "relay/upstream.hpp"
#include "testing.hpp"

using namespace relay;

namespace {

ClusterSnapshotPtr MakeCluster(int count, std::vector<uint32_t> weights = {}) {
    std::vector<Endpoint> eps;
    for (int i = 0; i < count; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = i < static_cast<int>(weights.size()) ? weights[i] : 1u;
        ep.health = Health::kHealthy;
        eps.push_back(ep);
    }
    return std::make_shared<ClusterSnapshot>("c", std::move(eps));
}

std::map<size_t, int> Distribute(BalancerKind kind, ClusterSnapshotPtr cluster, int n,
                                 bool complete_immediately = true) {
    Balancer b(kind);
    b.SetSnapshot(cluster);
    std::map<size_t, int> counts;
    for (int i = 0; i < n; ++i) {
        auto idx = b.Pick(Fnv1a64("k" + std::to_string(i)));
        if (!idx.has_value()) continue;
        counts[*idx]++;
        b.OnRequestStart(*idx);
        if (complete_immediately) b.OnRequestEnd(*idx, false);
    }
    return counts;
}

double WorstDeviation(const std::map<size_t, int>& counts, int total, int endpoints) {
    double fair = static_cast<double>(total) / endpoints;
    double worst = 0;
    for (const auto& [idx, n] : counts) worst = std::max(worst, std::abs(n - fair) / fair);
    return worst;
}

}  // namespace

RELAY_TEST(empty_cluster_picks_nothing) {
    auto cluster = std::make_shared<ClusterSnapshot>("c", std::vector<Endpoint>{});
    for (auto kind : {BalancerKind::kRoundRobin, BalancerKind::kLeastRequest,
                      BalancerKind::kP2C, BalancerKind::kRandom, BalancerKind::kRingHash}) {
        Balancer b(kind);
        b.SetSnapshot(cluster);
        CHECK(!b.Pick(123).has_value());
    }
}

RELAY_TEST(no_snapshot_picks_nothing) {
    Balancer b(BalancerKind::kP2C);
    CHECK(!b.Pick(1).has_value());
}

RELAY_TEST(round_robin_is_exactly_even) {
    auto counts = Distribute(BalancerKind::kRoundRobin, MakeCluster(4), 400);
    CHECK_EQ(counts.size(), size_t(4));
    for (const auto& [idx, n] : counts) CHECK_EQ(n, 100);
}

RELAY_TEST(round_robin_honours_weight) {
    auto counts = Distribute(BalancerKind::kRoundRobin, MakeCluster(3, {3, 1, 1}), 500);
    // Weight 3 of total 5 is 60%.
    double share = static_cast<double>(counts[0]) / 500;
    if (share < 0.55 || share > 0.65) {
        std::fprintf(stderr, "        weighted endpoint got %.1f%%\n", share * 100);
        CHECK(false);
    }
}

RELAY_TEST(random_is_roughly_even) {
    const int n = 100000;
    auto counts = Distribute(BalancerKind::kRandom, MakeCluster(8), n);
    CHECK_EQ(counts.size(), size_t(8));
    double worst = WorstDeviation(counts, n, 8);
    if (worst > 0.05) {
        std::fprintf(stderr, "        random worst deviation %.2f%%\n", worst * 100);
        CHECK(false);
    }
}

RELAY_TEST(p2c_is_roughly_even_when_load_is_uniform) {
    const int n = 100000;
    auto counts = Distribute(BalancerKind::kP2C, MakeCluster(8), n);
    CHECK_EQ(counts.size(), size_t(8));
    double worst = WorstDeviation(counts, n, 8);
    if (worst > 0.06) {
        std::fprintf(stderr, "        p2c worst deviation %.2f%%\n", worst * 100);
        CHECK(false);
    }
}

RELAY_TEST(p2c_picks_two_distinct_endpoints) {
    // Without distinctness the degenerate same-pick case silently degrades to
    // plain random, which is measurably worse and invisible in aggregate.
    // Two endpoints: a correct P2C always compares both, so with one loaded
    // it must always choose the other.
    Balancer b(BalancerKind::kP2C);
    b.SetSnapshot(MakeCluster(2));
    b.OnRequestStart(0);
    b.OnRequestStart(0);
    b.OnRequestStart(0);
    for (int i = 0; i < 50; ++i) {
        auto idx = b.Pick();
        CHECK(idx.has_value());
        CHECK_EQ(*idx, size_t(1));
    }
}

RELAY_TEST(least_request_prefers_the_idle_endpoint) {
    Balancer b(BalancerKind::kLeastRequest);
    b.SetSnapshot(MakeCluster(3));
    // Load up endpoints 0 and 1.
    for (int i = 0; i < 5; ++i) {
        b.OnRequestStart(0);
        b.OnRequestStart(1);
    }
    auto idx = b.Pick();
    CHECK(idx.has_value());
    CHECK_EQ(*idx, size_t(2));
}

RELAY_TEST(least_request_honours_weight) {
    // An endpoint with twice the weight should carry twice the load before it
    // looks equally busy.
    Balancer b(BalancerKind::kLeastRequest);
    b.SetSnapshot(MakeCluster(2, {2, 1}));
    b.OnRequestStart(0);
    b.OnRequestStart(0);
    b.OnRequestStart(1);
    // 2 active at weight 2 vs 1 active at weight 1: equal normalised load, so
    // the tie goes to the first. Add one more to endpoint 1 and endpoint 0
    // must win.
    b.OnRequestStart(1);
    auto idx = b.Pick();
    CHECK(idx.has_value());
    CHECK_EQ(*idx, size_t(0));
}

RELAY_TEST(least_request_balances_under_sustained_load) {
    // The behaviour that distinguishes it from round robin: requests that do
    // not complete immediately should pile up evenly rather than all landing
    // on one endpoint.
    Balancer b(BalancerKind::kLeastRequest);
    b.SetSnapshot(MakeCluster(4));
    std::map<size_t, int> counts;
    for (int i = 0; i < 400; ++i) {
        auto idx = b.Pick();
        CHECK(idx.has_value());
        counts[*idx]++;
        b.OnRequestStart(*idx);  // Never completed.
    }
    for (const auto& [idx, n] : counts) CHECK_EQ(n, 100);
}

RELAY_TEST(unhealthy_endpoints_are_never_picked) {
    std::vector<Endpoint> eps;
    for (int i = 0; i < 5; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = 1;
        ep.health = (i % 2 == 0) ? Health::kHealthy : Health::kUnhealthy;
        eps.push_back(ep);
    }
    auto cluster = std::make_shared<ClusterSnapshot>("c", eps);
    for (auto kind : {BalancerKind::kRoundRobin, BalancerKind::kLeastRequest,
                      BalancerKind::kP2C, BalancerKind::kRandom, BalancerKind::kRingHash}) {
        Balancer b(kind);
        b.SetSnapshot(cluster);
        for (int i = 0; i < 500; ++i) {
            auto idx = b.Pick(Fnv1a64("k" + std::to_string(i)));
            CHECK(idx.has_value());
            if (*idx % 2 != 0) {
                std::fprintf(stderr, "        %s picked unhealthy endpoint %zu\n",
                             BalancerKindName(kind), *idx);
                CHECK(false);
                break;
            }
        }
    }
}

RELAY_TEST(unknown_health_is_not_eligible) {
    // A newly added endpoint that has never been probed must not receive
    // traffic on faith.
    std::vector<Endpoint> eps;
    Endpoint a;
    a.host = "10.0.0.1"; a.port = 8080; a.weight = 1; a.health = Health::kHealthy;
    Endpoint b;
    b.host = "10.0.0.2"; b.port = 8080; b.weight = 1; b.health = Health::kUnknown;
    eps = {a, b};
    auto cluster = std::make_shared<ClusterSnapshot>("c", eps);
    CHECK_EQ(cluster->eligible_count(), size_t(1));

    Balancer bal(BalancerKind::kRoundRobin);
    bal.SetSnapshot(cluster);
    for (int i = 0; i < 20; ++i) CHECK_EQ(*bal.Pick(), size_t(0));
}

RELAY_TEST(active_count_returns_to_zero) {
    Balancer b(BalancerKind::kP2C);
    b.SetSnapshot(MakeCluster(3));
    for (int i = 0; i < 100; ++i) {
        auto idx = b.Pick();
        b.OnRequestStart(*idx);
        b.OnRequestEnd(*idx, i % 3 == 0);
    }
    for (size_t i = 0; i < 3; ++i) CHECK_EQ(b.load(i).active, uint64_t(0));
}

RELAY_TEST(request_end_without_start_does_not_underflow) {
    // A missed OnRequestStart from an early return would otherwise wrap the
    // counter to 2^64, and least-request would never choose that endpoint
    // again. Silently never routing to a healthy endpoint is much worse than
    // an off-by-one count.
    Balancer b(BalancerKind::kLeastRequest);
    b.SetSnapshot(MakeCluster(2));
    b.OnRequestEnd(0, false);
    b.OnRequestEnd(0, false);
    CHECK_EQ(b.load(0).active, uint64_t(0));
    // And the endpoint is still choosable.
    bool saw_zero = false;
    for (int i = 0; i < 20; ++i) {
        if (*b.Pick() == 0) saw_zero = true;
    }
    CHECK(saw_zero);
}

RELAY_TEST(out_of_range_index_is_ignored) {
    Balancer b(BalancerKind::kP2C);
    b.SetSnapshot(MakeCluster(2));
    b.OnRequestStart(99);
    b.OnRequestEnd(99, true);
    CHECK_EQ(b.load(99).active, uint64_t(0));
}

RELAY_TEST(snapshot_swap_preserves_load_prefix) {
    // Dropping in-flight counts on a config push would make every endpoint
    // look idle and the next few hundred requests would pile onto whichever
    // sorted first.
    Balancer b(BalancerKind::kLeastRequest);
    b.SetSnapshot(MakeCluster(2));
    for (int i = 0; i < 5; ++i) b.OnRequestStart(0);
    CHECK_EQ(b.load(0).active, uint64_t(5));

    b.SetSnapshot(MakeCluster(3));  // An endpoint was appended.
    CHECK_EQ(b.load(0).active, uint64_t(5));
    // The loaded endpoint must not win. Endpoints 1 and 2 are both idle, so
    // which of them is chosen is a tie the balancer may break either way —
    // asserting a specific one would be asserting an implementation detail.
    CHECK(*b.Pick() != size_t(0));
}

RELAY_TEST(ring_hash_gives_affinity) {
    Balancer b(BalancerKind::kRingHash);
    b.SetSnapshot(MakeCluster(5));
    for (int i = 0; i < 100; ++i) {
        uint64_t key = Fnv1a64("session-" + std::to_string(i));
        auto first = b.Pick(key);
        CHECK(first.has_value());
        for (int j = 0; j < 5; ++j) CHECK_EQ(b.Pick(key), first);
    }
}

RELAY_TEST(deterministic_from_seed) {
    // Two balancers with the same seed must make identical choices, or no
    // distribution measurement reproduces.
    auto cluster = MakeCluster(6);
    Balancer a(BalancerKind::kP2C, 42);
    Balancer b(BalancerKind::kP2C, 42);
    a.SetSnapshot(cluster);
    b.SetSnapshot(cluster);
    for (int i = 0; i < 200; ++i) CHECK_EQ(a.Pick(), b.Pick());
}

RELAY_TEST(zero_seed_does_not_wedge_the_rng) {
    // xorshift stuck at zero presents as every pick landing on endpoint 0.
    Balancer b(BalancerKind::kRandom, 0);
    b.SetSnapshot(MakeCluster(4));
    std::map<size_t, int> counts;
    for (int i = 0; i < 400; ++i) counts[*b.Pick()]++;
    CHECK(counts.size() > 1);
}

RELAY_TEST_MAIN()
