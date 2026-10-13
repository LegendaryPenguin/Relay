#pragma once

// Metrics: counters and a latency histogram.
//
// The histogram is the part worth explaining. A proxy's useful latency
// question is "what do the slowest 1% of requests see", and the two obvious
// implementations both answer it wrongly:
//
//   * **A mean** hides everything. A p50 of 2ms with a p99 of 4s averages to
//     something that looks fine and describes nobody's experience.
//   * **Keeping every sample** and sorting is exact and unbounded. At 50k
//     requests/sec a one-minute window is three million doubles, and the
//     sort is on whoever reads the metrics.
//
// So: fixed log-linear buckets. Bounded memory, constant-time recording, and
// percentile error bounded by bucket width rather than by sample count — which
// is the right trade for a number an operator reads to two significant
// figures.
//
// What this is *not* is mergeable across processes with exact percentiles.
// Bucket counts merge exactly, so a merged histogram's percentiles are as good
// as a single one's, but neither is exact. That is stated here because a
// histogram presented as exact is worse than one presented honestly.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace relay {

// Log-linear latency histogram over microseconds.
//
// 1us to ~65s. Each power of two is divided into kSubBuckets linear steps, so
// relative error is bounded at roughly 1/kSubBuckets — about 6% at 16
// sub-buckets, which is finer than the run-to-run variance of any latency
// measurement this proxy makes.
class LatencyHistogram {
  public:
    static constexpr size_t kBuckets = 26;      // Powers of two: 1us .. ~67s.
    static constexpr size_t kSubBuckets = 16;
    static constexpr size_t kTotalBuckets = kBuckets * kSubBuckets;

    void Record(uint64_t micros);

    uint64_t count() const { return count_; }
    uint64_t sum_micros() const { return sum_; }
    uint64_t max_micros() const { return max_; }

    // Percentile in [0,1]. Returns microseconds at the containing bucket's
    // upper edge, so reported values are an over-estimate rather than an
    // under-estimate. Over-estimating is the safer direction for a latency
    // SLO: it never claims the system is faster than it is.
    uint64_t Percentile(double p) const;

    double mean_micros() const {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // Adds another histogram's counts. Exact for counts, which is what makes
    // per-thread histograms viable — each worker records into its own and a
    // reader merges, so recording needs no atomics at all.
    void Merge(const LatencyHistogram& other);

    void Reset();

  private:
    static size_t BucketFor(uint64_t micros);
    static uint64_t UpperBound(size_t bucket);

    std::array<uint64_t, kTotalBuckets> buckets_{};
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t max_ = 0;
};

// Per-cluster counters.
//
// Plain integers, not atomics. Each worker thread owns its own Stats and a
// reader merges snapshots, which keeps the request path free of contended
// cache lines. The cost is that a reader sees a very slightly inconsistent
// view across counters — acceptable for metrics, and not for anything that
// makes decisions.
struct ClusterStats {
    uint64_t requests = 0;
    uint64_t responses_2xx = 0;
    uint64_t responses_4xx = 0;
    uint64_t responses_5xx = 0;

    uint64_t upstream_failures = 0;
    uint64_t timeouts = 0;
    uint64_t retries = 0;
    uint64_t retries_denied_by_budget = 0;

    uint64_t breaker_rejections = 0;
    uint64_t rate_limited = 0;
    uint64_t load_shed = 0;
    uint64_t no_healthy_upstream = 0;

    uint64_t pool_hits = 0;
    uint64_t pool_misses = 0;
    uint64_t connections_opened = 0;
    uint64_t connections_closed = 0;

    LatencyHistogram total_latency;     // Client-observed, including retries.
    LatencyHistogram upstream_latency;  // One attempt.

    void Merge(const ClusterStats& other);

    // Renders Prometheus-style text. Hand-written rather than using a client
    // library: the exposition format is a dozen lines of printing and the
    // library would be the largest dependency in the data plane.
    std::string Render(const std::string& cluster) const;
};

// Snapshot of gauges that are only meaningful as instantaneous values.
struct GaugeSnapshot {
    uint32_t in_flight = 0;
    uint32_t queue_depth = 0;
    uint32_t pooled_connections = 0;
    uint32_t active_connections = 0;
    const char* breaker_state = "closed";
    double retry_budget_tokens = 0;
};

}  // namespace relay
