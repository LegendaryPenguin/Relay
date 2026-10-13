#include "relay/policy.hpp"

#include <algorithm>
#include <cmath>

#include "relay/upstream.hpp"

namespace relay {

namespace {

double SecondsBetween(TimePoint a, TimePoint b) {
    return std::chrono::duration<double>(b - a).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// CircuitBreaker
// ---------------------------------------------------------------------------

const char* CircuitBreaker::state_name() const {
    switch (state_) {
        case State::kClosed: return "closed";
        case State::kOpen: return "open";
        case State::kHalfOpen: return "half_open";
    }
    return "invalid";
}

bool CircuitBreaker::Allow(TimePoint now) {
    switch (state_) {
        case State::kClosed:
            return true;

        case State::kOpen:
            if (now - opened_at_ >= config_.open_duration) {
                // Transition to half-open and admit this request as the first
                // probe. Advancing the state here rather than in a separate
                // tick is why Allow must be called before every attempt.
                state_ = State::kHalfOpen;
                consecutive_successes_ = 0;
                half_open_inflight_ = 1;
                return true;
            }
            ++rejected_;
            return false;

        case State::kHalfOpen:
            if (half_open_inflight_ >= config_.half_open_probes) {
                // Bounded probing. Letting unlimited requests through while
                // half-open sends the full load at an upstream that has not
                // yet proven it recovered, which is the thundering herd the
                // half-open state exists to prevent.
                ++rejected_;
                return false;
            }
            ++half_open_inflight_;
            return true;
    }
    return true;
}

void CircuitBreaker::OnSuccess(TimePoint now) {
    (void)now;
    ++total_requests_;
    consecutive_failures_ = 0;

    if (state_ == State::kHalfOpen) {
        if (half_open_inflight_ > 0) --half_open_inflight_;
        ++consecutive_successes_;
        if (consecutive_successes_ >= config_.success_threshold) Close();
    }
}

void CircuitBreaker::OnFailure(TimePoint now) {
    ++total_requests_;
    ++consecutive_failures_;
    consecutive_successes_ = 0;

    if (state_ == State::kHalfOpen) {
        // A single failure while probing re-opens immediately. Requiring
        // several would keep sending trial traffic at an upstream that has
        // already said no.
        if (half_open_inflight_ > 0) --half_open_inflight_;
        Open(now);
        return;
    }

    if (state_ == State::kClosed) {
        if (total_requests_ < config_.minimum_requests) {
            // Not enough evidence. A cluster that has served three requests
            // and failed all three has not demonstrated anything.
            return;
        }
        if (consecutive_failures_ >= config_.failure_threshold) Open(now);
    }
}

void CircuitBreaker::Open(TimePoint now) {
    state_ = State::kOpen;
    opened_at_ = now;
    half_open_inflight_ = 0;
    consecutive_successes_ = 0;
    ++opened_count_;
}

void CircuitBreaker::Close() {
    state_ = State::kClosed;
    consecutive_failures_ = 0;
    consecutive_successes_ = 0;
    half_open_inflight_ = 0;
}

// ---------------------------------------------------------------------------
// RetryBudget
// ---------------------------------------------------------------------------

void RetryBudget::Refill(TimePoint now) {
    if (!initialised_) {
        last_refill_ = now;
        initialised_ = true;
        return;
    }
    double elapsed = SecondsBetween(last_refill_, now);
    if (elapsed <= 0) return;
    last_refill_ = now;
    // The floor rate accrues with time regardless of traffic, so a cluster
    // serving one request a minute can still retry it. Without this the budget
    // is unusable at low volume: the ratio earns nothing from a single
    // success and the first failure is never retried.
    tokens_ = std::min(config_.burst, tokens_ + elapsed * config_.min_per_second);
}

void RetryBudget::OnRequest(bool success, TimePoint now) {
    Refill(now);
    if (success) {
        // Budget is earned from successes only. This is the property that
        // makes the budget bound aggregate amplification: during a total
        // outage there are no successes, so the bucket drains and retries
        // stop — the opposite of a per-request count, which triples load
        // exactly then.
        tokens_ = std::min(config_.burst, tokens_ + config_.ratio);
    }
}

bool RetryBudget::TryConsume(TimePoint now) {
    Refill(now);
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        ++granted_;
        return true;
    }
    ++denied_;
    return false;
}

// ---------------------------------------------------------------------------
// RateLimiter
// ---------------------------------------------------------------------------

RateLimiter::Bucket* RateLimiter::FindOrCreate(std::string_view key, TimePoint now) {
    if (buckets_.empty()) {
        // Power of two so the mask is cheap; capacity is fixed at first use
        // rather than grown, because rehashing on the request path is the
        // kind of latency spike that shows up only under the load that caused
        // it.
        size_t capacity = 1;
        while (capacity < config_.max_keys) capacity <<= 1;
        buckets_.resize(capacity);
    }

    uint64_t h = Fnv1a64(key);
    size_t mask = buckets_.size() - 1;
    size_t slot = h & mask;

    // Linear probing over a short span. A long probe sequence on the hot path
    // is worse than an approximate answer, so after kProbe steps the entry is
    // replaced. That makes the limiter approximate under key pressure: a
    // client can evict another client's bucket and briefly get a fresh
    // allowance. The alternative — unbounded tracking — lets a rotating
    // identifier exhaust memory, which turns the limiter into an
    // amplification vector. Stated in the README as a real limitation.
    constexpr size_t kProbe = 4;
    size_t victim_slot = slot;
    double victim_tokens = -1;
    for (size_t i = 0; i < kProbe; ++i) {
        size_t s = (slot + i) & mask;
        Bucket& b = buckets_[s];
        if (b.key == key) return &b;
        if (b.key.empty()) {
            b.key = std::string(key);
            b.tokens = config_.burst;
            b.last_refill = now;
            return &b;
        }
        // Choose the *most idle* bucket as the victim — the one with the most
        // tokens left.
        //
        // Evicting an arbitrary slot instead meant a hot key was constantly
        // thrown out and re-admitted with a fresh allowance, so under key
        // pressure the limiter stopped limiting the one client it most needed
        // to. A hot key has few tokens and now survives; a key that arrived
        // once and went away has a full bucket and is the right thing to drop.
        double projected = std::min(
            config_.burst, b.tokens + SecondsBetween(b.last_refill, now) * config_.per_second);
        if (projected > victim_tokens) {
            victim_tokens = projected;
            victim_slot = s;
        }
    }

    Bucket& victim = buckets_[victim_slot];
    victim.key = std::string(key);
    victim.tokens = config_.burst;
    victim.last_refill = now;
    return &victim;
}

bool RateLimiter::Allow(std::string_view key, TimePoint now) {
    Bucket* b = FindOrCreate(key, now);
    double elapsed = SecondsBetween(b->last_refill, now);
    if (elapsed > 0) {
        b->last_refill = now;
        b->tokens = std::min(config_.burst, b->tokens + elapsed * config_.per_second);
    }
    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        ++allowed_;
        return true;
    }
    ++limited_;
    return false;
}

// ---------------------------------------------------------------------------
// LoadShedder
// ---------------------------------------------------------------------------

double LoadShedder::ShedProbability(uint32_t queue_depth) const {
    // The hard limit is checked first. With soft_threshold at 1.0 the two
    // boundaries coincide, and testing the soft one first made
    // ShedProbability(max_queue_depth) return 0 — the proxy admitting
    // everything at precisely the depth it was configured to stop at.
    if (queue_depth >= config_.max_queue_depth) return 1.0;
    double soft = config_.soft_threshold * config_.max_queue_depth;
    if (static_cast<double>(queue_depth) <= soft) return 0.0;
    double span = config_.max_queue_depth - soft;
    if (span <= 0) return 1.0;
    // Linear ramp from the soft threshold to the hard limit. Linear rather
    // than a cliff because a hard threshold makes the system oscillate
    // between fully accepting and fully rejecting, and that oscillation is
    // itself a latency spike.
    return (static_cast<double>(queue_depth) - soft) / span;
}

bool LoadShedder::Admit(uint32_t in_flight, uint32_t queue_depth, double roll) {
    if (in_flight >= config_.max_concurrent) {
        ++shed_;
        return false;
    }
    double p = ShedProbability(queue_depth);
    if (p > 0.0 && roll < p) {
        ++shed_;
        return false;
    }
    ++admitted_;
    return true;
}

void LoadShedder::Reset() {
    shed_ = 0;
    admitted_ = 0;
}

// ---------------------------------------------------------------------------
// RetryPolicy
// ---------------------------------------------------------------------------

bool RetryPolicy::ShouldRetry(uint32_t attempts_made, const Status& status,
                              bool idempotent) const {
    if (attempts_made >= config_.max_attempts) return false;
    if (!status.retryable()) return false;
    // A timeout on a non-idempotent request is not retried. The request may
    // have been applied and only the response lost, and applying a POST twice
    // is a correctness failure where giving up is merely an availability one.
    if (status.code() == Code::kTimeout && !idempotent) return false;
    return true;
}

Millis RetryPolicy::Backoff(uint32_t attempt, double roll) const {
    double base = static_cast<double>(config_.initial_backoff.count());
    for (uint32_t i = 1; i < attempt; ++i) {
        base *= config_.multiplier;
        if (base >= static_cast<double>(config_.max_backoff.count())) break;
    }
    base = std::min(base, static_cast<double>(config_.max_backoff.count()));

    // Full jitter in [base*(1-j), base]. Without jitter every client that
    // failed together retries together, turning a brief hiccup into a
    // standing wave of synchronised bursts that keeps the upstream down.
    double low = base * (1.0 - config_.jitter);
    double value = low + roll * (base - low);
    return Millis(static_cast<long long>(value));
}

}  // namespace relay
