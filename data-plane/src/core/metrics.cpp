#include "relay/metrics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <sstream>

namespace relay {

size_t LatencyHistogram::BucketFor(uint64_t micros) {
    if (micros == 0) return 0;
    // The power-of-two band, then a linear offset within it. std::bit_width
    // gives the band in one instruction, which matters because this runs on
    // every request.
    int band = std::bit_width(micros) - 1;  // 0 for 1us, 1 for 2-3us, ...
    if (band < 0) band = 0;
    if (band >= static_cast<int>(kBuckets)) {
        return kTotalBuckets - 1;  // Saturate rather than wrap.
    }
    uint64_t band_start = 1ull << band;
    uint64_t band_width = band_start;  // Each band spans [2^n, 2^(n+1)).
    uint64_t offset = micros - band_start;
    size_t sub = static_cast<size_t>((offset * kSubBuckets) / band_width);
    if (sub >= kSubBuckets) sub = kSubBuckets - 1;
    return static_cast<size_t>(band) * kSubBuckets + sub;
}

uint64_t LatencyHistogram::UpperBound(size_t bucket) {
    size_t band = bucket / kSubBuckets;
    size_t sub = bucket % kSubBuckets;
    uint64_t band_start = 1ull << band;
    uint64_t band_width = band_start;
    return band_start + ((sub + 1) * band_width) / kSubBuckets;
}

void LatencyHistogram::Record(uint64_t micros) {
    buckets_[BucketFor(micros)]++;
    ++count_;
    sum_ += micros;
    max_ = std::max(max_, micros);
}

uint64_t LatencyHistogram::Percentile(double p) const {
    if (count_ == 0) return 0;
    if (p <= 0.0) return 0;
    if (p >= 1.0) return max_;

    // Ceiling, not truncation. The comment here used to promise a ceiling
    // while the code truncated, which made p99 of two samples read as p50 —
    // a percentile that reports one sample too optimistic, which is the
    // single most misleading way for a latency metric to be wrong.
    uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_)));
    if (target == 0) target = 1;

    uint64_t cumulative = 0;
    for (size_t i = 0; i < kTotalBuckets; ++i) {
        cumulative += buckets_[i];
        if (cumulative >= target) {
            // The bucket's upper edge, capped at the observed max. Without the
            // cap a p99 can exceed the largest sample, which is confusing and
            // indefensible even if it is within the error bound.
            return std::min(UpperBound(i), max_);
        }
    }
    return max_;
}

void LatencyHistogram::Merge(const LatencyHistogram& other) {
    for (size_t i = 0; i < kTotalBuckets; ++i) buckets_[i] += other.buckets_[i];
    count_ += other.count_;
    sum_ += other.sum_;
    max_ = std::max(max_, other.max_);
}

void LatencyHistogram::Reset() {
    buckets_.fill(0);
    count_ = 0;
    sum_ = 0;
    max_ = 0;
}

void ClusterStats::Merge(const ClusterStats& o) {
    requests += o.requests;
    responses_2xx += o.responses_2xx;
    responses_4xx += o.responses_4xx;
    responses_5xx += o.responses_5xx;
    upstream_failures += o.upstream_failures;
    timeouts += o.timeouts;
    retries += o.retries;
    retries_denied_by_budget += o.retries_denied_by_budget;
    breaker_rejections += o.breaker_rejections;
    rate_limited += o.rate_limited;
    load_shed += o.load_shed;
    no_healthy_upstream += o.no_healthy_upstream;
    pool_hits += o.pool_hits;
    pool_misses += o.pool_misses;
    connections_opened += o.connections_opened;
    connections_closed += o.connections_closed;
    total_latency.Merge(o.total_latency);
    upstream_latency.Merge(o.upstream_latency);
}

std::string ClusterStats::Render(const std::string& cluster) const {
    std::ostringstream out;
    auto counter = [&](const char* name, uint64_t value) {
        out << "relay_" << name << "{cluster=\"" << cluster << "\"} " << value << "\n";
    };

    counter("requests_total", requests);
    counter("responses_2xx_total", responses_2xx);
    counter("responses_4xx_total", responses_4xx);
    counter("responses_5xx_total", responses_5xx);
    counter("upstream_failures_total", upstream_failures);
    counter("timeouts_total", timeouts);
    counter("retries_total", retries);
    counter("retries_denied_by_budget_total", retries_denied_by_budget);
    counter("breaker_rejections_total", breaker_rejections);
    counter("rate_limited_total", rate_limited);
    counter("load_shed_total", load_shed);
    counter("no_healthy_upstream_total", no_healthy_upstream);
    counter("pool_hits_total", pool_hits);
    counter("pool_misses_total", pool_misses);
    counter("connections_opened_total", connections_opened);
    counter("connections_closed_total", connections_closed);

    // Percentiles are exported as a gauge-per-quantile rather than as a
    // Prometheus histogram. A real histogram export would be more useful to a
    // server that wants to re-aggregate; this is what an operator reads
    // directly, and the bucket counts are not exported because 416 series per
    // cluster is more cardinality than the information justifies.
    auto quantile = [&](const char* name, const LatencyHistogram& h, double p) {
        out << "relay_" << name << "{cluster=\"" << cluster << "\",quantile=\"" << p << "\"} "
            << h.Percentile(p) << "\n";
    };
    for (double p : {0.5, 0.9, 0.99, 0.999}) {
        quantile("total_latency_micros", total_latency, p);
    }
    for (double p : {0.5, 0.9, 0.99, 0.999}) {
        quantile("upstream_latency_micros", upstream_latency, p);
    }
    out << "relay_total_latency_micros_count{cluster=\"" << cluster << "\"} "
        << total_latency.count() << "\n";
    out << "relay_total_latency_micros_sum{cluster=\"" << cluster << "\"} "
        << total_latency.sum_micros() << "\n";

    return out.str();
}

}  // namespace relay
