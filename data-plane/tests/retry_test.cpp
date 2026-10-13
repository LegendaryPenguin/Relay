#include "relay/http.hpp"
#include "relay/policy.hpp"
#include "testing.hpp"

using namespace relay;

namespace {

TimePoint T0() { return TimePoint{} + Millis(1000000); }

}  // namespace

RELAY_TEST(budget_starts_full_and_drains) {
    RetryBudget::Config cfg;
    cfg.ratio = 0.2;
    cfg.min_per_second = 0;  // Isolate the ratio.
    cfg.burst = 5;
    RetryBudget b(cfg);
    TimePoint now = T0();

    int granted = 0;
    for (int i = 0; i < 20; ++i) {
        if (b.TryConsume(now)) ++granted;
    }
    CHECK_EQ(granted, 5);
    CHECK_EQ(b.denied(), uint64_t(15));
}

RELAY_TEST(budget_is_earned_only_from_successes) {
    // The property that makes the budget bound aggregate amplification:
    // during a total outage there are no successes, so the bucket drains and
    // retries stop — the opposite of a per-request count, which triples load
    // exactly then.
    RetryBudget::Config cfg;
    cfg.ratio = 0.5;
    cfg.min_per_second = 0;
    cfg.burst = 2;
    RetryBudget b(cfg);
    TimePoint now = T0();

    // Drain.
    while (b.TryConsume(now)) {}
    CHECK(b.tokens() < 1.0);

    // Failures earn nothing.
    for (int i = 0; i < 50; ++i) b.OnRequest(false, now);
    CHECK(!b.TryConsume(now));

    // Successes do: two at ratio 0.5 is one token.
    b.OnRequest(true, now);
    b.OnRequest(true, now);
    CHECK(b.TryConsume(now));
}

RELAY_TEST(total_outage_produces_fewer_retries_than_partial) {
    // Stated as a comparison because it is the whole argument for a budget
    // over a count. Same number of requests; one scenario fails everything,
    // the other fails a tenth.
    auto run = [](double failure_rate) {
        RetryBudget::Config cfg;
        cfg.ratio = 0.2;
        cfg.min_per_second = 0;
        cfg.burst = 10;
        RetryBudget b(cfg);
        TimePoint now = T0();
        int retries = 0;
        for (int i = 0; i < 1000; ++i) {
            bool failed = (i % 100) < static_cast<int>(failure_rate * 100);
            b.OnRequest(!failed, now);
            if (failed && b.TryConsume(now)) ++retries;
        }
        return retries;
    };

    int partial = run(0.1);
    int total = run(1.0);
    if (!(total < partial)) {
        std::fprintf(stderr, "        total outage retries=%d, partial=%d\n", total, partial);
        CHECK(false);
    }
}

RELAY_TEST(floor_rate_lets_low_traffic_clusters_retry) {
    // Without a floor the budget is unusable at low volume: the ratio earns
    // nothing from a single success and the first failure is never retried.
    RetryBudget::Config cfg;
    cfg.ratio = 0.2;
    cfg.min_per_second = 3;
    cfg.burst = 10;
    RetryBudget b(cfg);
    TimePoint now = T0();
    while (b.TryConsume(now)) {}
    CHECK(!b.TryConsume(now));

    // One second of accrual at 3/sec.
    CHECK(b.TryConsume(now + Millis(1000)));
}

RELAY_TEST(budget_is_capped_at_burst) {
    RetryBudget::Config cfg;
    cfg.ratio = 1.0;
    cfg.min_per_second = 100;
    cfg.burst = 4;
    RetryBudget b(cfg);
    TimePoint now = T0();
    for (int i = 0; i < 100; ++i) b.OnRequest(true, now + Millis(i * 100));
    CHECK(b.tokens() <= 4.0);
}

RELAY_TEST(retry_policy_respects_attempt_cap) {
    RetryPolicy::Config cfg;
    cfg.max_attempts = 3;
    RetryPolicy p(cfg);
    auto fail = Status::UpstreamFailure("boom");
    CHECK(p.ShouldRetry(1, fail, true));
    CHECK(p.ShouldRetry(2, fail, true));
    CHECK(!p.ShouldRetry(3, fail, true));
    CHECK(!p.ShouldRetry(4, fail, true));
}

RELAY_TEST(non_retryable_statuses_are_not_retried) {
    RetryPolicy p;
    CHECK(!p.ShouldRetry(1, Status::Ok(), true));
    CHECK(!p.ShouldRetry(1, Status::InvalidRequest("bad"), true));
    // Shedding must not be retried through the same proxy: that adds load to
    // a component that has already said it has none to spare.
    CHECK(!p.ShouldRetry(1, Status::Overloaded("shed"), true));
    CHECK(!p.ShouldRetry(1, Status::NoHealthyUpstream("none"), true));
    CHECK(!p.ShouldRetry(1, Status::Internal("bug"), true));
}

RELAY_TEST(timeout_on_non_idempotent_request_is_not_retried) {
    // A timeout cannot distinguish "never arrived" from "arrived and the
    // reply was lost". Applying a POST twice is a correctness failure where
    // giving up is merely an availability one.
    RetryPolicy p;
    CHECK(p.ShouldRetry(1, Status::Timeout("slow"), /*idempotent=*/true));
    CHECK(!p.ShouldRetry(1, Status::Timeout("slow"), /*idempotent=*/false));
    // A connection failure, by contrast, is safe to retry either way: the
    // request was never delivered.
    CHECK(p.ShouldRetry(1, Status::UpstreamFailure("refused"), false));
}

RELAY_TEST(backoff_grows_and_is_capped) {
    RetryPolicy::Config cfg;
    cfg.initial_backoff = Millis(10);
    cfg.max_backoff = Millis(100);
    cfg.multiplier = 2.0;
    cfg.jitter = 0.0;  // Isolate the growth curve.
    RetryPolicy p(cfg);

    CHECK_EQ(p.Backoff(1, 0.0).count(), 10);
    CHECK_EQ(p.Backoff(2, 0.0).count(), 20);
    CHECK_EQ(p.Backoff(3, 0.0).count(), 40);
    CHECK_EQ(p.Backoff(4, 0.0).count(), 80);
    CHECK_EQ(p.Backoff(5, 0.0).count(), 100);
    CHECK_EQ(p.Backoff(50, 0.0).count(), 100);
}

RELAY_TEST(jitter_spreads_backoff) {
    // Not optional. Without it every client that failed together retries
    // together, turning a brief hiccup into a standing wave of synchronised
    // bursts that keeps the upstream down.
    RetryPolicy::Config cfg;
    cfg.initial_backoff = Millis(100);
    cfg.max_backoff = Millis(100);
    cfg.jitter = 0.5;
    RetryPolicy p(cfg);

    // roll=0 gives the low end, roll=1 the high end.
    CHECK_EQ(p.Backoff(1, 0.0).count(), 50);
    CHECK_EQ(p.Backoff(1, 1.0).count(), 100);
    CHECK_EQ(p.Backoff(1, 0.5).count(), 75);
    // Every value stays inside the band.
    for (int i = 0; i <= 100; ++i) {
        auto ms = p.Backoff(1, i / 100.0).count();
        CHECK(ms >= 50 && ms <= 100);
    }
}

RELAY_TEST_MAIN()
