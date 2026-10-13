#include "relay/policy.hpp"
#include "testing.hpp"

using namespace relay;

namespace {

// A virtual clock. Nothing in the policy layer reads the wall clock, which is
// what makes every transition here exact rather than timing-dependent.
TimePoint T0() { return TimePoint{} + Millis(1000000); }

CircuitBreaker::Config FastConfig() {
    CircuitBreaker::Config c;
    c.failure_threshold = 3;
    c.minimum_requests = 5;
    c.open_duration = Millis(1000);
    c.half_open_probes = 2;
    c.success_threshold = 2;
    return c;
}

void Succeed(CircuitBreaker& b, TimePoint now, int n) {
    for (int i = 0; i < n; ++i) {
        CHECK(b.Allow(now));
        b.OnSuccess(now);
    }
}

}  // namespace

RELAY_TEST(starts_closed_and_allows) {
    CircuitBreaker b(FastConfig());
    CHECK_EQ(b.state(), CircuitBreaker::State::kClosed);
    CHECK(b.Allow(T0()));
}

RELAY_TEST(does_not_open_before_minimum_requests) {
    // A cluster that has served three requests and failed all three has not
    // demonstrated anything. Without the minimum, a breaker opens on no
    // evidence during startup.
    CircuitBreaker b(FastConfig());
    TimePoint now = T0();
    for (int i = 0; i < 4; ++i) {
        CHECK(b.Allow(now));
        b.OnFailure(now);
    }
    CHECK_EQ(b.state(), CircuitBreaker::State::kClosed);
}

RELAY_TEST(opens_after_threshold_consecutive_failures) {
    CircuitBreaker b(FastConfig());
    TimePoint now = T0();
    Succeed(b, now, 5);  // Clear the minimum-requests gate.
    for (int i = 0; i < 3; ++i) {
        CHECK(b.Allow(now));
        b.OnFailure(now);
    }
    CHECK_EQ(b.state(), CircuitBreaker::State::kOpen);
    CHECK_EQ(b.opened_count(), uint64_t(1));
    // And it now rejects without an attempt.
    CHECK(!b.Allow(now));
    CHECK_EQ(b.rejected(), uint64_t(1));
}

RELAY_TEST(a_success_resets_the_consecutive_count) {
    // Consecutive, not cumulative. An upstream failing 1-in-3 forever should
    // not eventually trip a consecutive-failure breaker.
    CircuitBreaker b(FastConfig());
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int round = 0; round < 20; ++round) {
        CHECK(b.Allow(now));
        b.OnFailure(now);
        CHECK(b.Allow(now));
        b.OnFailure(now);
        CHECK(b.Allow(now));
        b.OnSuccess(now);
    }
    CHECK_EQ(b.state(), CircuitBreaker::State::kClosed);
}

RELAY_TEST(stays_open_for_the_configured_duration) {
    auto cfg = FastConfig();
    CircuitBreaker b(cfg);
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int i = 0; i < 3; ++i) { b.Allow(now); b.OnFailure(now); }
    CHECK_EQ(b.state(), CircuitBreaker::State::kOpen);

    // Just before the deadline: still open.
    CHECK(!b.Allow(now + cfg.open_duration - Millis(1)));
    CHECK_EQ(b.state(), CircuitBreaker::State::kOpen);

    // At the deadline: transitions to half-open and admits a probe.
    CHECK(b.Allow(now + cfg.open_duration));
    CHECK_EQ(b.state(), CircuitBreaker::State::kHalfOpen);
}

RELAY_TEST(half_open_admits_only_bounded_probes) {
    // Letting unlimited requests through while half-open sends the full load
    // at an upstream that has not proven it recovered — the thundering herd
    // the half-open state exists to prevent.
    auto cfg = FastConfig();
    CircuitBreaker b(cfg);
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int i = 0; i < 3; ++i) { b.Allow(now); b.OnFailure(now); }

    TimePoint later = now + cfg.open_duration;
    CHECK(b.Allow(later));   // probe 1
    CHECK(b.Allow(later));   // probe 2
    CHECK(!b.Allow(later));  // over the limit
    CHECK(!b.Allow(later));
}

RELAY_TEST(half_open_closes_after_success_threshold) {
    auto cfg = FastConfig();
    CircuitBreaker b(cfg);
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int i = 0; i < 3; ++i) { b.Allow(now); b.OnFailure(now); }

    TimePoint later = now + cfg.open_duration;
    CHECK(b.Allow(later));
    b.OnSuccess(later);
    CHECK_EQ(b.state(), CircuitBreaker::State::kHalfOpen);
    CHECK(b.Allow(later));
    b.OnSuccess(later);
    CHECK_EQ(b.state(), CircuitBreaker::State::kClosed);
    // Fully closed means unbounded admission again.
    for (int i = 0; i < 10; ++i) CHECK(b.Allow(later));
}

RELAY_TEST(one_failure_while_half_open_reopens_immediately) {
    // Requiring several would keep sending trial traffic at an upstream that
    // has already said no.
    auto cfg = FastConfig();
    CircuitBreaker b(cfg);
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int i = 0; i < 3; ++i) { b.Allow(now); b.OnFailure(now); }

    TimePoint later = now + cfg.open_duration;
    CHECK(b.Allow(later));
    b.OnFailure(later);
    CHECK_EQ(b.state(), CircuitBreaker::State::kOpen);
    CHECK_EQ(b.opened_count(), uint64_t(2));
    CHECK(!b.Allow(later));
}

RELAY_TEST(half_open_success_does_not_leak_probe_slots) {
    // The probe counter must be decremented on completion, or after a couple
    // of half-open cycles no probe is ever admitted again and the breaker is
    // stuck open forever.
    auto cfg = FastConfig();
    CircuitBreaker b(cfg);
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int i = 0; i < 3; ++i) { b.Allow(now); b.OnFailure(now); }

    TimePoint t = now + cfg.open_duration;
    // Probe, succeed once (not enough to close), then re-probe repeatedly.
    for (int cycle = 0; cycle < 5; ++cycle) {
        CHECK(b.Allow(t));
        b.OnSuccess(t);
        if (b.state() == CircuitBreaker::State::kClosed) break;
    }
    CHECK_EQ(b.state(), CircuitBreaker::State::kClosed);
}

RELAY_TEST(recovers_fully_after_reopening) {
    auto cfg = FastConfig();
    CircuitBreaker b(cfg);
    TimePoint now = T0();
    Succeed(b, now, 5);
    for (int i = 0; i < 3; ++i) { b.Allow(now); b.OnFailure(now); }

    // First probe fails, reopening.
    TimePoint t1 = now + cfg.open_duration;
    CHECK(b.Allow(t1));
    b.OnFailure(t1);
    CHECK_EQ(b.state(), CircuitBreaker::State::kOpen);

    // Second window: probes succeed and it closes.
    TimePoint t2 = t1 + cfg.open_duration;
    CHECK(b.Allow(t2));
    b.OnSuccess(t2);
    CHECK(b.Allow(t2));
    b.OnSuccess(t2);
    CHECK_EQ(b.state(), CircuitBreaker::State::kClosed);
    CHECK_EQ(b.consecutive_failures(), uint32_t(0));
}

RELAY_TEST(state_names_are_stable) {
    // These end up in metrics labels, so a rename is an operator-visible
    // break.
    CircuitBreaker b;
    CHECK_EQ(std::string(b.state_name()), std::string("closed"));
}

RELAY_TEST(shed_load_must_not_charge_the_breaker) {
    // The coupling that matters most across the whole policy layer, asserted
    // at the Status level since that is where the router consults it.
    CHECK(!Status::Overloaded("shed").charges_breaker());
    CHECK(!Status::Overloaded("shed").retryable());
    // And the failures that should charge it do.
    CHECK(Status::UpstreamFailure("conn refused").charges_breaker());
    CHECK(Status::Timeout("slow").charges_breaker());
    // A bad client request is not the upstream's fault.
    CHECK(!Status::InvalidRequest("bad").charges_breaker());
    // Nor is an empty cluster.
    CHECK(!Status::NoHealthyUpstream("none").charges_breaker());
    CHECK(!Status::NoHealthyUpstream("none").retryable());
}

RELAY_TEST(status_http_mapping) {
    CHECK_EQ(Status::Ok().http_status(), 200);
    CHECK_EQ(Status::InvalidRequest("x").http_status(), 400);
    CHECK_EQ(Status::UpstreamFailure("x").http_status(), 502);
    CHECK_EQ(Status::Timeout("x").http_status(), 504);
    CHECK_EQ(Status::Overloaded("x").http_status(), 503);
    CHECK_EQ(Status::NoHealthyUpstream("x").http_status(), 503);
    CHECK_EQ(Status::Internal("x").http_status(), 500);
}

RELAY_TEST_MAIN()
