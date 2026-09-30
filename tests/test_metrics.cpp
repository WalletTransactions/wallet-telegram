// util/metrics: гистограмма с одним лейблом (observe_duration) — формат
// экспозиции Prometheus, который читает дашборд.
#include "util/metrics.hpp"

#include "test_framework.hpp"

namespace {

bool has(const std::string& text, const std::string& line) { return text.find(line) != std::string::npos; }

} // namespace

TEST(metrics_observe_duration_renders_cumulative_buckets) {
    metrics::observe_duration("test_duration_seconds", "Тест.", "method", "sendMessage", 0.02);
    metrics::observe_duration("test_duration_seconds", "Тест.", "method", "sendMessage", 3.0);
    metrics::observe_duration("test_duration_seconds", "Тест.", "method", "editMessageText", 20.0);
    std::string out = metrics::render();

    CHECK(has(out, "# TYPE test_duration_seconds histogram\n"));
    // Кумулятивно: 0.02 попадает во все бакеты от 0.025, 3.0 — от 5.
    CHECK(has(out, "test_duration_seconds_bucket{method=\"sendMessage\",le=\"0.01\"} 0\n"));
    CHECK(has(out, "test_duration_seconds_bucket{method=\"sendMessage\",le=\"0.025\"} 1\n"));
    CHECK(has(out, "test_duration_seconds_bucket{method=\"sendMessage\",le=\"2.5\"} 1\n"));
    CHECK(has(out, "test_duration_seconds_bucket{method=\"sendMessage\",le=\"5\"} 2\n"));
    CHECK(has(out, "test_duration_seconds_bucket{method=\"sendMessage\",le=\"+Inf\"} 2\n"));
    CHECK(has(out, "test_duration_seconds_count{method=\"sendMessage\"} 2\n"));
    CHECK(has(out, "test_duration_seconds_sum{method=\"sendMessage\"} 3.02\n"));
    // Больше последнего бакета — только в +Inf.
    CHECK(has(out, "test_duration_seconds_bucket{method=\"editMessageText\",le=\"10\"} 0\n"));
    CHECK(has(out, "test_duration_seconds_bucket{method=\"editMessageText\",le=\"+Inf\"} 1\n"));
}
