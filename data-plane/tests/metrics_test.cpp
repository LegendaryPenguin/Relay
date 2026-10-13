#include <string>
#include <vector>

#include "relay/metrics.hpp"
#include "testing.hpp"

using namespace relay;

RELAY_TEST(empty_histogram_reports_zero) {
    LatencyHistogram h;
    CHECK_EQ(h.count(), uint64_t(0));
    CHECK_EQ(h.Percentile(0.5), uint64_t(0));
    CHECK_EQ(h.mean_micros(), 0.0);
}

RELAY_TEST(single_sample) {
    LatencyHistogram h;
    h.Record(1000);
    CHECK_EQ(h.count(), uint64_t(1));
    CHECK_EQ(h.max_micros(), uint64_t(1000));
    // The bucket's upper edge, capped at the observed max.
    CHECK_EQ(h.Percentile(0.5), uint64_t(1000));
}

RELAY_TEST(percentiles_are_within_the_error_bound) {
    // Log-linear buckets bound relative error at roughly 1/kSubBuckets, about
    // 6% at 16. Asserting a tighter bound would be asserting something the
    // structure does not provide.
    LatencyHistogram h;
    for (int i = 1; i <= 10000; ++i) h.Record(static_cast<uint64_t>(i));

    struct Case { double p; uint64_t expect; };
    const std::vector<Case> cases = {{0.5, 5000}, {0.9, 9000}, {0.99, 9900}};
    for (const auto& c : cases) {
        uint64_t got = h.Percentile(c.p);
        double error = std::abs(static_cast<double>(got) - c.expect) / c.expect;
        if (error > 0.08) {
            std::fprintf(stderr, "        p%.0f = %llu, expected ~%llu (%.1f%% error)\n",
                         c.p * 100, (unsigned long long)got, (unsigned long long)c.expect,
                         error * 100);
            CHECK(false);
        }
    }
}

RELAY_TEST(percentile_never_exceeds_the_max) {
    // A p99 above the largest observed sample is confusing and indefensible
    // even when it is within the bucket's error bound.
    LatencyHistogram h;
    h.Record(3);
    h.Record(7);
    h.Record(11);
    for (double p : {0.5, 0.9, 0.99, 0.999, 1.0}) {
        CHECK(h.Percentile(p) <= h.max_micros());
    }
}

RELAY_TEST(percentile_uses_a_ceiling_not_truncation) {
    // The classic off-by-one. With two samples, p99 must be the larger one:
    // ceil(0.99 * 2) = 2, so the 2nd sample. Truncation gives 1 and reports
    // p99 as the median, which is a latency metric lying in the most
    // flattering direction.
    LatencyHistogram h;
    h.Record(10);
    h.Record(1000000);
    CHECK(h.Percentile(0.99) > 1000);
}

RELAY_TEST(p99_reflects_the_99th_sample_not_the_outlier_count) {
    // 99 small samples and one huge one: p99 is correctly the *small* value,
    // because the 99th of 100 sorted samples is small. The outlier belongs to
    // max and p100. Asserting otherwise would be asserting a wrong
    // definition, which is how a percentile implementation gets "fixed" into
    // being wrong.
    LatencyHistogram h;
    for (int i = 0; i < 99; ++i) h.Record(10);
    h.Record(1000000);
    CHECK(h.Percentile(0.99) <= 20);
    CHECK_EQ(h.max_micros(), uint64_t(1000000));

    // With 5% outliers, p99 does land on one.
    LatencyHistogram g;
    for (int i = 0; i < 95; ++i) g.Record(10);
    for (int i = 0; i < 5; ++i) g.Record(1000000);
    CHECK(g.Percentile(0.99) > 1000);
}

RELAY_TEST(wide_dynamic_range) {
    LatencyHistogram h;
    h.Record(1);
    h.Record(1000);
    h.Record(1000000);
    h.Record(60000000);
    CHECK_EQ(h.count(), uint64_t(4));
    CHECK_EQ(h.max_micros(), uint64_t(60000000));
    CHECK(h.Percentile(0.5) < h.Percentile(0.99));
}

RELAY_TEST(saturates_rather_than_wrapping) {
    // A value past the top bucket must land in the top bucket, not wrap to
    // the bottom one and report a microsecond.
    LatencyHistogram h;
    h.Record(1);
    h.Record(UINT64_MAX / 2);
    CHECK(h.Percentile(0.99) > 1000);
}

RELAY_TEST(zero_is_recordable) {
    LatencyHistogram h;
    h.Record(0);
    CHECK_EQ(h.count(), uint64_t(1));
    CHECK_EQ(h.max_micros(), uint64_t(0));
}

RELAY_TEST(merge_is_exact_for_counts) {
    // What makes per-thread histograms viable: recording needs no atomics
    // because a reader merges snapshots.
    LatencyHistogram a, b;
    for (int i = 1; i <= 500; ++i) a.Record(static_cast<uint64_t>(i));
    for (int i = 501; i <= 1000; ++i) b.Record(static_cast<uint64_t>(i));

    LatencyHistogram merged;
    merged.Merge(a);
    merged.Merge(b);
    CHECK_EQ(merged.count(), uint64_t(1000));
    CHECK_EQ(merged.sum_micros(), a.sum_micros() + b.sum_micros());
    CHECK_EQ(merged.max_micros(), uint64_t(1000));

    LatencyHistogram direct;
    for (int i = 1; i <= 1000; ++i) direct.Record(static_cast<uint64_t>(i));
    // Merged and direct must agree exactly, since both are bucket counts.
    for (double p : {0.5, 0.9, 0.99}) CHECK_EQ(merged.Percentile(p), direct.Percentile(p));
}

RELAY_TEST(reset_clears) {
    LatencyHistogram h;
    h.Record(100);
    h.Reset();
    CHECK_EQ(h.count(), uint64_t(0));
    CHECK_EQ(h.max_micros(), uint64_t(0));
}

RELAY_TEST(cluster_stats_merge) {
    ClusterStats a, b;
    a.requests = 10;
    a.responses_2xx = 8;
    a.retries = 2;
    a.total_latency.Record(500);
    b.requests = 5;
    b.responses_2xx = 5;
    b.total_latency.Record(1500);

    a.Merge(b);
    CHECK_EQ(a.requests, uint64_t(15));
    CHECK_EQ(a.responses_2xx, uint64_t(13));
    CHECK_EQ(a.retries, uint64_t(2));
    CHECK_EQ(a.total_latency.count(), uint64_t(2));
}

RELAY_TEST(render_includes_the_cluster_label_and_key_series) {
    ClusterStats s;
    s.requests = 42;
    s.load_shed = 7;
    s.total_latency.Record(1234);
    std::string out = s.Render("api");

    CHECK(out.find("cluster=\"api\"") != std::string::npos);
    CHECK(out.find("relay_requests_total") != std::string::npos);
    CHECK(out.find("42") != std::string::npos);
    CHECK(out.find("relay_load_shed_total") != std::string::npos);
    CHECK(out.find("quantile=\"0.99\"") != std::string::npos);
    // Bucket counts are deliberately not exported: 416 series per cluster is
    // more cardinality than the information justifies.
    CHECK(out.find("_bucket") == std::string::npos);
}

RELAY_TEST_MAIN()
