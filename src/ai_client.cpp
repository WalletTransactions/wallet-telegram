#include "ai_client.hpp"

#include "httplib.h"
#include "util/log.hpp"
#include "util/metrics.hpp"

using nlohmann::json;

namespace ai {

namespace {

std::string error_of(const json& body, int status) {
    std::string e = "HTTP " + std::to_string(status);
    if (body.is_object() && body.contains("error") && body["error"].is_string()) e += ": " + body["error"].get<std::string>();
    return e;
}

void count_error(const std::string& kind) {
    metrics::inc_counter("tg_ai_errors_total", "Ошибки запросов бота к wallet-ai.", "kind", kind);
}

class HttpAiApi final : public AiApi {
public:
    explicit HttpAiApi(std::string base_url) : base_url_(std::move(base_url)) {}

    CreateResult create_parse_expense(const std::string& token, const json& input,
                                      const std::string& idempotency_key) override {
        json body = {{"kind", "parse_expense"}, {"input", input}, {"idempotency_key", idempotency_key}};
        httplib::Client cli = client();
        auto res = cli.Post("/api/ai/jobs", headers(token), body.dump(), "application/json");
        if (!res) {
            count_error("network");
            CreateResult r;
            r.error = "wallet-ai недоступен: " + httplib::to_string(res.error());
            return r;
        }
        return parse_create_response(res->status, res->body);
    }

    JobResult get_job(const std::string& token, const std::string& job_id) override {
        httplib::Client cli = client();
        auto res = cli.Get("/api/ai/jobs/" + job_id, headers(token));
        if (!res) {
            count_error("network");
            JobResult r;
            r.error = "wallet-ai недоступен: " + httplib::to_string(res.error());
            return r;
        }
        return parse_job_response(res->status, res->body);
    }

    void cancel_job(const std::string& token, const std::string& job_id) override {
        httplib::Client cli = client();
        auto res = cli.Delete("/api/ai/jobs/" + job_id, headers(token));
        if (!res) count_error("network");
    }

    std::optional<bool> enabled(const std::string& token) override {
        httplib::Client cli = client();
        auto res = cli.Get("/api/ai/status", headers(token));
        if (!res || res->status != 200) return std::nullopt;
        json body = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
        if (!body.is_object() || !body.contains("enabled") || !body["enabled"].is_boolean()) return std::nullopt;
        return body["enabled"].get<bool>();
    }

private:
    // Клиент на вызов: запросы короткие (бот не ждёт long-poll'ом), а у
    // httplib::Client нет гарантий на переиспользование после ошибок.
    httplib::Client client() const {
        httplib::Client cli(base_url_);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(10, 0); // создание задания ходит в wallet-report за справочником
        return cli;
    }

    static httplib::Headers headers(const std::string& token) {
        httplib::Headers h = {{"Authorization", "Bearer " + token}};
        std::string trace_id = applog::current_trace_id();
        if (!trace_id.empty()) h.emplace("X-Trace-Id", trace_id);
        return h;
    }

    std::string base_url_;
};

} // namespace

const char* to_string(CreateResult::Status s) {
    using S = CreateResult::Status;
    switch (s) {
    case S::Created: return "created";
    case S::Unavailable: return "unavailable";
    case S::Busy: return "busy";
    case S::Unauthorized: return "unauthorized";
    case S::Error: return "error";
    }
    return "error";
}

const char* to_string(JobResult::Status s) {
    using S = JobResult::Status;
    switch (s) {
    case S::Pending: return "pending";
    case S::Done: return "done";
    case S::Failed: return "failed";
    case S::Cancelled: return "cancelled";
    case S::NotFound: return "not_found";
    case S::Unauthorized: return "unauthorized";
    case S::Error: return "error";
    }
    return "error";
}

CreateResult parse_create_response(int http_status, const std::string& body_text) {
    CreateResult r;
    json body = json::parse(body_text, nullptr, /*allow_exceptions=*/false);
    if (http_status == 202 && body.is_object() && body.contains("job_id") && body["job_id"].is_string()) {
        r.status = CreateResult::Status::Created;
        r.job_id = body["job_id"].get<std::string>();
        return r;
    }
    r.error = error_of(body, http_status);
    if (http_status == 503) {
        r.status = CreateResult::Status::Unavailable;
    } else if (http_status == 429) {
        r.status = CreateResult::Status::Busy;
    } else if (http_status == 401) {
        r.status = CreateResult::Status::Unauthorized;
    } else {
        count_error("http_" + std::to_string(http_status));
        r.status = CreateResult::Status::Error;
    }
    return r;
}

JobResult parse_job_response(int http_status, const std::string& body_text) {
    JobResult r;
    json body = json::parse(body_text, nullptr, /*allow_exceptions=*/false);
    if (http_status == 404) {
        r.status = JobResult::Status::NotFound;
        return r;
    }
    if (http_status == 401) {
        r.status = JobResult::Status::Unauthorized;
        return r;
    }
    if (http_status != 200 || !body.is_object() || !body.contains("status") || !body["status"].is_string()) {
        if (http_status != 200) count_error("http_" + std::to_string(http_status));
        r.error = http_status == 200 ? "ответ wallet-ai не JSON" : error_of(body, http_status);
        return r;
    }
    std::string status = body["status"].get<std::string>();
    if (status == "queued" || status == "running") {
        r.status = JobResult::Status::Pending;
    } else if (status == "done" && body.contains("result") && body["result"].is_object()) {
        r.status = JobResult::Status::Done;
        r.result = body["result"];
    } else if (status == "failed") {
        r.status = JobResult::Status::Failed;
        r.failure = body.value("failure", std::string{});
    } else if (status == "cancelled") {
        r.status = JobResult::Status::Cancelled;
    } else {
        r.error = "неизвестный статус задания: " + status;
    }
    return r;
}

std::unique_ptr<AiApi> make_http_api(const std::string& base_url) { return std::make_unique<HttpAiApi>(base_url); }

} // namespace ai
