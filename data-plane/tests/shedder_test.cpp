#include "relay/policy.hpp"
#include "testing.hpp"

using namespace relay;

RELAY_TEST(admits_freely_below_the_soft_threshold) {
    LoadShedder::Config cfg;
    cfg.max_concurrent = 100;
    cfg.max_queue_depth = 100;
    cfg.soft_threshold = 0.5;
    LoadShedder s(cfg);

    for (uint32_t depth = 0; depth <= 50; ++depth) {
        CHECK_EQ(s.ShedProbability(depth), 0.0);
        CHECK(s.Admit(1, depth, 0.0));
        CHECK(s.Admit(1, depth, 0.99));
    }
}

RELAY_TEST(sheds_everything_at_the_hard_limit) {
    LoadShedder::Config cfg;
    cfg.max_concurrent = 100;
    cfg.max_queue_depth = 100;
    cfg.soft_threshold = 0.5;
    LoadShedder s(cfg);

    CHECK_EQ(s.ShedProbability(100), 1.0);
    CHECK_EQ(s.ShedProbability(200), 1.0);
    CHECK(!s.Admit(1, 100, 0.999));
}

RELAY_TEST(ramps_linearly_between_thresholds) {
    // Linear rather than a cliff: a hard threshold makes the system oscillate
    // between fully accepting and fully rejecting, and that oscillation is
    // itself a latency spike.
    LoadShedder::Config cfg;
    cfg.max_queue_depth = 100;
    cfg.soft_threshold = 0.5;
    LoadShedder s(cfg);

    // Halfway between 50 and 100 should shed about half.
    double p = s.ShedProbability(75);
    if (p < 0.49 || p > 0.51) {
        std::fprintf(stderr, "        p at depth 75 = %.3f, expected ~0.5\n", p);
        CHECK(false);
    }
    // Monotonic.
    double prev = -1;
    for (uint32_t d = 50; d <= 100; ++d) {
        double cur = s.ShedProbability(d);
        CHECK(cur >= prev);
        prev = cur;
    }
}

RELAY_TEST(concurrency_limit_is_a_hard_gate) {
    // A concurrency limit rather than a rate limit, because the thing being
    // protected is a finite number of connections and worker threads, and
    // requests-per-second bounds neither when latency varies.
    LoadShedder::Config cfg;
    cfg.max_concurrent = 10;
    cfg.max_queue_depth = 1000;
    LoadShedder s(cfg);

    CHECK(s.Admit(9, 0, 0.0));
    CHECK(!s.Admit(10, 0, 0.0));
    CHECK(!s.Admit(50, 0, 0.0));
}

RELAY_TEST(shedding_is_deterministic_given_the_roll) {
    // The roll is a parameter rather than drawn internally so the curve can
    // be tested exactly instead of statistically.
    LoadShedder::Config cfg;
    cfg.max_queue_depth = 100;
    cfg.soft_threshold = 0.5;
    LoadShedder s(cfg);

    // At depth 75 the probability is 0.5.
    CHECK(!s.Admit(1, 75, 0.25));  // below p: shed
    CHECK(s.Admit(1, 75, 0.75));   // above p: admit
}

RELAY_TEST(counters_add_up) {
    LoadShedder::Config cfg;
    cfg.max_queue_depth = 100;
    cfg.soft_threshold = 0.5;
    LoadShedder s(cfg);
    for (int i = 0; i < 100; ++i) s.Admit(1, 75, i / 100.0);
    CHECK_EQ(s.admitted() + s.shed(), uint64_t(100));
    // Roughly half each at p=0.5.
    CHECK(s.shed() > 40 && s.shed() < 60);
}

RELAY_TEST(soft_threshold_of_one_means_no_ramp) {
    LoadShedder::Config cfg;
    cfg.max_queue_depth = 100;
    cfg.soft_threshold = 1.0;
    LoadShedder s(cfg);
    CHECK_EQ(s.ShedProbability(99), 0.0);
    CHECK_EQ(s.ShedProbability(100), 1.0);
}

RELAY_TEST(reset_clears_counters) {
    LoadShedder s;
    s.Admit(0, 0, 0.0);
    s.Reset();
    CHECK_EQ(s.admitted(), uint64_t(0));
    CHECK_EQ(s.shed(), uint64_t(0));
}

RELAY_TEST_MAIN()
