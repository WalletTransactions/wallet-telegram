// util/metrics: гистограмма с одним лейблом (observe_duration) — формат
// экспозиции Prometheus, который читает дашборд.
#include "util/metrics.hpp"

#include "test_framework.hpp"

#include <ctime>
#include <string>

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

// build_info с меткой build и время запуска: started_at_iso() (GET /version)
// и process_start_time_seconds (/metrics) — одна и та же секунда.
TEST(metrics_build_info_and_start_time) {
    std::time_t before = std::time(nullptr);
    metrics::set_build_info("wallet-telegram", "0.1.0", "0.1.0-rc.2", "a1b2c3d4");
    std::time_t after = std::time(nullptr);
    std::string out = metrics::render();

    CHECK(has(out, "build_info{service=\"wallet-telegram\",version=\"0.1.0\",build=\"0.1.0-rc.2\","
                   "git_commit=\"a1b2c3d4\"} 1\n"));
    CHECK(has(out, "# TYPE process_start_time_seconds gauge\n"));

    std::size_t pos = out.find("\nprocess_start_time_seconds ");
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    std::time_t started = static_cast<std::time_t>(std::stoll(out.substr(pos + 28)));
    CHECK(started >= before && started <= after);

    std::tm tm{};
    gmtime_r(&started, &tm);
    char want[32];
    std::strftime(want, sizeof want, "%Y-%m-%dT%H:%M:%SZ", &tm);
    CHECK(metrics::started_at_iso() == want);
}
