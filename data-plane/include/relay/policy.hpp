#pragma once

// Failure-protection policy: circuit breakers, retry budgets, rate limiting,
// load shedding.
//
// These four are in one header because they have to be designed together, and
// that is the main thing I took from building this. Each one, alone, is a
// reasonable idea that makes an outage worse:
//
//   * **Retries alone** multiply load exactly when the upstream is least able
//     to take it. Three attempts per request means a struggling upstream gets
//     3x traffic at the moment it starts failing.
//   * **A circuit breaker alone** converts a partial failure into a total one
//     for the breaker's duration, and if every proxy trips simultaneously the
//     upstream gets a thundering herd when they all probe at once.
//   * **Rate limiting alone** rejects traffic the system could have served,
//     because a fixed limit cannot know the current capacity.
//   * **Load shedding alone** needs a signal, and the obvious signal (latency)
//     lags the overload it is meant to detect.
//
// Together they work because each bounds a different failure mode: the budget
// bounds retry amplification, the breaker bounds the cost of a dead upstream,
// the limiter bounds a single client, and shedding bounds queueing. The
// coupling that matters most and is easiest to get wrong: **shed load must not
// charge the circuit breaker.** Shedding is the proxy protecting the upstream;
// if it trips the breaker, self-protection becomes a self-inflicted outage.

#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "relay/status.hpp"

namespace relay {

// Each policy class below has a separate default constructor that delegates
// to the parameterised one, rather than a `Config config = {}` default
// argument. A default argument cannot aggregate-initialise a nested Config
// whose members have default initialisers: the enclosing class is not complete
// at that point in its own definition. A delegating constructor's
// mem-initialiser list is a complete-class context, so it can.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Millis = std::chrono::milliseconds;

// Circuit breaker.
//
// Three states, and the half-open one is the whole point. A breaker that only
// opened and closed on a timer would send the full load at a possibly-still-
// dead upstream the instant the timer expired. Half-open admits a bounded
// number of trial requests and decides from those.
class CircuitBreaker {
  public:
    enum class State {
        kClosed,    // Normal. Requests pass.
        kOpen,      // Failing. Requests rejected without an attempt.
        kHalfOpen,  // Probing. A bounded number of trials admitted.
    };

    struct Config {
        // Consecutive failures before opening. Consecutive rather than a
        // windowed rate because it is cheap, has no warm-up, and cannot be
        // fooled by a long quiet period — a rate over a sparse window swings
        // wildly on low traffic and trips breakers on idle clusters.
        uint32_t failure_threshold = 5;
        // Minimum requests before the breaker may open at all. Without it, a
        // cluster that has served three requests and failed all three gets
        // its breaker opened on no evidence.
        uint32_t minimum_requests = 10;
        // How long to stay open before probing.
        Millis open_duration{5000};
        // Trials admitted while half-open.
        uint32_t half_open_probes = 3;
        // Consecutive successes while half-open needed to close.
        uint32_t success_threshold = 2;
    };

    CircuitBreaker() : CircuitBreaker(Config{}) {}
    explicit CircuitBreaker(Config config) : config_(config) {}

    // Whether a request may be attempted now. Advances the state machine's
    // time-based transition, so it must be called before every attempt.
    bool Allow(TimePoint now);

    void OnSuccess(TimePoint now);
    void OnFailure(TimePoint now);

    State state() const { return state_; }
    const char* state_name() const;
    uint32_t consecutive_failures() const { return consecutive_failures_; }
    uint64_t rejected() const { return rejected_; }
    uint64_t opened_count() const { return opened_count_; }

  private:
    void Open(TimePoint now);
    void Close();

    Config config_;
    State state_ = State::kClosed;
    uint32_t consecutive_failures_ = 0;
    uint32_t consecutive_successes_ = 0;
    uint32_t half_open_inflight_ = 0;
    uint64_t total_requests_ = 0;
    uint64_t rejected_ = 0;
    uint64_t opened_count_ = 0;
    TimePoint opened_at_{};
};

// Printable so test assertions and log lines can name the state rather than
// its integer value. Found by ADL on CircuitBreaker::State.
inline std::ostream& operator<<(std::ostream& os, CircuitBreaker::State s) {
    switch (s) {
        case CircuitBreaker::State::kClosed: return os << "closed";
        case CircuitBreaker::State::kOpen: return os << "open";
        case CircuitBreaker::State::kHalfOpen: return os << "half_open";
    }
    return os << "invalid";
}

// Retry budget: a token bucket that bounds retries as a *fraction* of
// successful traffic.
//
// This replaced a per-request retry count, and the difference is the single
// most important thing in this file. Three attempts per request bounds the
// amplification per request at 3x and bounds it in aggregate at nothing: when
// every request starts failing, total load triples precisely when the upstream
// can least absorb it. A budget expressed as "retries may be at most 20% of
// successful requests" bounds the aggregate, so a total outage produces
// *fewer* retries rather than more, because there are no successes to earn
// budget from.
class RetryBudget {
  public:
    struct Config {
        // Retries permitted per successful request, as a ratio.
        double ratio = 0.2;
        // Minimum retries per second regardless of success volume, so a
        // low-traffic cluster can still retry at all.
        double min_per_second = 3.0;
        // Bucket capacity in tokens, bounding burstiness.
        double burst = 20.0;
    };

    RetryBudget() : RetryBudget(Config{}) {}
    explicit RetryBudget(Config config) : config_(config), tokens_(config.burst) {}

    // Records a completed attempt, earning budget on success.
    void OnRequest(bool success, TimePoint now);

    // Whether a retry may be attempted, consuming a token if so.
    bool TryConsume(TimePoint now);

    double tokens() const { return tokens_; }
    uint64_t denied() const { return denied_; }
    uint64_t granted() const { return granted_; }

  private:
    void Refill(TimePoint now);

    Config config_;
    double tokens_;
    uint64_t denied_ = 0;
    uint64_t granted_ = 0;
    TimePoint last_refill_{};
    bool initialised_ = false;
};

// Token-bucket rate limiter, per key.
//
// Keyed so a single client cannot consume a shared limit. The key is whatever
// the caller chooses — remote address, a header, a route — and the limiter does
// not care, which keeps policy out of the mechanism.
class RateLimiter {
  public:
    struct Config {
        double per_second = 1000.0;
        double burst = 2000.0;
        // Bound on tracked keys. Unbounded would let a client with a rotating
        // identifier exhaust memory, which turns a rate limiter into a
        // denial-of-service amplifier.
        size_t max_keys = 65536;
    };

    RateLimiter() : RateLimiter(Config{}) {}
    explicit RateLimiter(Config config) : config_(config) {}

    bool Allow(std::string_view key, TimePoint now);

    size_t tracked_keys() const { return buckets_.size(); }
    uint64_t allowed() const { return allowed_; }
    uint64_t limited() const { return limited_; }

  private:
    struct Bucket {
        std::string key;
        double tokens = 0;
        TimePoint last_refill{};
    };

    Bucket* FindOrCreate(std::string_view key, TimePoint now);

    Config config_;
    // Open-addressed table rather than unordered_map: this is on the hot path
    // of every request, and a flat array of buckets with linear probing avoids
    // a pointer chase and an allocation per new key. Eviction is
    // replace-on-collision, which is approximate — see the README.
    std::vector<Bucket> buckets_;
    uint64_t allowed_ = 0;
    uint64_t limited_ = 0;
};

// Load shedder: refuses work before queueing makes latency unbounded.
//
// The signal is **queue depth**, not latency. That choice is the result of
// getting it wrong first: latency is a lagging indicator, so by the time
// p99 latency rises the queue is already deep and every request in it is
// already doomed. Queue depth leads, because it rises the moment arrival rate
// exceeds service rate.
//
// Concurrency limit rather than a rate limit, because the thing being
// protected is a finite number of upstream connections and worker threads,
// and "requests per second" does not bound either when latency varies.
class LoadShedder {
  public:
    struct Config {
        // Maximum requests in flight, across all upstreams.
        uint32_t max_concurrent = 1024;
        // Maximum queued-but-not-started requests.
        uint32_t max_queue_depth = 256;
        // Begin probabilistic shedding at this fraction of max_queue_depth.
        //
        // Gradual rather than a cliff: a hard threshold makes the system
        // oscillate between fully accepting and fully rejecting, and the
        // oscillation is itself a latency spike. Shedding a growing fraction
        // degrades smoothly.
        double soft_threshold = 0.5;
    };

    LoadShedder() : LoadShedder(Config{}) {}
    explicit LoadShedder(Config config) : config_(config) {}

    // Whether to admit a request. Deterministic given `roll` in [0,1).
    //
    // The roll is a parameter rather than drawn internally so the shedding
    // curve can be tested exactly instead of statistically.
    bool Admit(uint32_t in_flight, uint32_t queue_depth, double roll);

    // The fraction of requests that would be shed at a given depth. Exposed
    // for the tests and for operator reporting.
    double ShedProbability(uint32_t queue_depth) const;

    uint64_t shed() const { return shed_; }
    uint64_t admitted() const { return admitted_; }
    void Reset();

  private:
    Config config_;
    uint64_t shed_ = 0;
    uint64_t admitted_ = 0;
};

// Retry policy: how many attempts, with what backoff, for which failures.
class RetryPolicy {
  public:
    struct Config {
        uint32_t max_attempts = 3;
        Millis initial_backoff{10};
        Millis max_backoff{500};
        double multiplier = 2.0;
        // Jitter as a fraction of the computed backoff.
        //
        // Not optional. Without jitter, every client that failed together
        // retries together, and a brief upstream hiccup becomes a standing
        // wave of synchronised retry bursts that keeps it down.
        double jitter = 0.5;
    };

    RetryPolicy() : RetryPolicy(Config{}) {}
    explicit RetryPolicy(Config config) : config_(config) {}

    // Whether another attempt is permitted. `idempotent` gates retrying after
    // a timeout: a timeout cannot distinguish "never arrived" from "arrived
    // and the reply was lost", so retrying a non-idempotent request risks
    // applying it twice.
    bool ShouldRetry(uint32_t attempts_made, const Status& status, bool idempotent) const;

    // Backoff before the given attempt, with jitter applied from `roll`.
    Millis Backoff(uint32_t attempt, double roll) const;

    uint32_t max_attempts() const { return config_.max_attempts; }

  private:
    Config config_;
};

}  // namespace relay
