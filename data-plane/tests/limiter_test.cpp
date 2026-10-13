#include <string>

#include "relay/policy.hpp"
#include "testing.hpp"

using namespace relay;

namespace {
TimePoint T0() { return TimePoint{} + Millis(1000000); }
}  // namespace

RELAY_TEST(allows_up_to_burst_then_limits) {
    RateLimiter::Config cfg;
    cfg.per_second = 10;
    cfg.burst = 5;
    RateLimiter l(cfg);
    TimePoint now = T0();

    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (l.Allow("client-a", now)) ++allowed;
    }
    CHECK_EQ(allowed, 5);
    CHECK_EQ(l.limited(), uint64_t(15));
}

RELAY_TEST(refills_over_time) {
    RateLimiter::Config cfg;
    cfg.per_second = 10;
    cfg.burst = 5;
    RateLimiter l(cfg);
    TimePoint now = T0();
    while (l.Allow("a", now)) {}

    // 100ms at 10/sec is one token.
    CHECK(l.Allow("a", now + Millis(100)));
    CHECK(!l.Allow("a", now + Millis(100)));
    // A full second restores the burst.
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (l.Allow("a", now + Millis(1100))) ++allowed;
    }
    CHECK_EQ(allowed, 5);
}

RELAY_TEST(keys_are_independent) {
    // The whole point of keying: one client cannot consume another's
    // allowance.
    RateLimiter::Config cfg;
    cfg.per_second = 1;
    cfg.burst = 2;
    RateLimiter l(cfg);
    TimePoint now = T0();

    CHECK(l.Allow("noisy", now));
    CHECK(l.Allow("noisy", now));
    CHECK(!l.Allow("noisy", now));

    // A different client is unaffected.
    CHECK(l.Allow("quiet", now));
    CHECK(l.Allow("quiet", now));
}

RELAY_TEST(key_count_is_bounded) {
    // Unbounded tracking lets a client with a rotating identifier exhaust
    // memory, which turns a rate limiter into a denial-of-service amplifier.
    RateLimiter::Config cfg;
    cfg.per_second = 100;
    cfg.burst = 100;
    cfg.max_keys = 64;
    RateLimiter l(cfg);
    TimePoint now = T0();

    for (int i = 0; i < 10000; ++i) l.Allow("key-" + std::to_string(i), now);
    // The table is fixed-size, so tracked keys cannot exceed the capacity
    // chosen at first use.
    CHECK(l.tracked_keys() <= 128);
}

RELAY_TEST(eviction_under_key_pressure_is_approximate_not_fatal) {
    // Documented behaviour: a client can evict another's bucket and briefly
    // get a fresh allowance. The alternative is unbounded memory. This test
    // pins that it degrades rather than breaking — the limiter still limits a
    // single hot key while cold keys churn around it.
    RateLimiter::Config cfg;
    cfg.per_second = 0.001;  // Effectively no refill.
    cfg.burst = 3;
    cfg.max_keys = 16;
    RateLimiter l(cfg);
    TimePoint now = T0();

    int hot_allowed = 0;
    for (int round = 0; round < 50; ++round) {
        if (l.Allow("hot", now)) ++hot_allowed;
        // Churn cold keys to apply pressure.
        for (int i = 0; i < 4; ++i) l.Allow("cold-" + std::to_string(round * 4 + i), now);
    }
    // Without eviction it would be exactly 3. With eviction it may be more,
    // but must be far below 50 or the limiter is not limiting at all.
    if (hot_allowed >= 25) {
        std::fprintf(stderr, "        hot key allowed %d of 50 under key pressure\n",
                     hot_allowed);
        CHECK(false);
    }
}

RELAY_TEST(counters_add_up) {
    RateLimiter::Config cfg;
    cfg.per_second = 1;
    cfg.burst = 3;
    RateLimiter l(cfg);
    TimePoint now = T0();
    for (int i = 0; i < 10; ++i) l.Allow("a", now);
    CHECK_EQ(l.allowed() + l.limited(), uint64_t(10));
    CHECK_EQ(l.allowed(), uint64_t(3));
}

RELAY_TEST(zero_elapsed_time_does_not_refill) {
    // A clock that does not advance must not grant free tokens, or a busy
    // loop within one clock tick bypasses the limit entirely.
    RateLimiter::Config cfg;
    cfg.per_second = 1000000;
    cfg.burst = 2;
    RateLimiter l(cfg);
    TimePoint now = T0();
    CHECK(l.Allow("a", now));
    CHECK(l.Allow("a", now));
    CHECK(!l.Allow("a", now));
}

RELAY_TEST_MAIN()
