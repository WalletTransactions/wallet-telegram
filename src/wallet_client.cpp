#include "wallet_client.hpp"

#include <chrono>

#include "httplib.h"
#include "util/log.hpp"
#include "util/metrics.hpp"

using nlohmann::json;

namespace wallet {

namespace {

void count_error(const std::string& kind) {
    metrics::inc_counter("tg_wallet_api_errors_total", "Ошибки запросов бота к wallet-api.", "kind", kind);
}

class HttpWalletApi final : public WalletApi {
public:
    explicit HttpWalletApi(const std::string& base_url) : base_url_(base_url) {}

    PushResult push_expense(const Expense& e, const std::string& access_token) override {
        // Клиент на вызов, как в reports_client.cpp.
        httplib::Client client(base_url_);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(15, 0);

        httplib::Headers headers = {{"Authorization", "Bearer " + access_token}};
        std::string trace_id = applog::current_trace_id();
        if (!trace_id.empty()) headers.emplace("X-Trace-Id", trace_id);

        auto started = std::chrono::steady_clock::now();
        auto res = client.Post("/sync/push", headers, push_body(e).dump(), "application/json");
        PushResult out;
        if (!res) {
            count_error("network");
            out.error = "wallet-api недоступен: " + httplib::to_string(res.error());
        } else {
            if (res->status != 200) count_error("http_" + std::to_string(res->status));
            out = parse_push_response(res->status, res->body);
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        metrics::observe_duration("tg_wallet_push_duration_seconds",
                                  "Длительность записи траты в wallet-api (POST /sync/push) по исходу.", "status",
                                  status_name(out.status), seconds);
        return out;
    }

private:
    std::string base_url_;
};

} // namespace

json push_body(const Expense& e) {
    json op = {
        {"uuid", e.op_uuid},
        {"date", e.date},
        {"acc_fr_uuid", e.wallet_uuid},
        {"acc_to_uuid", e.category_uuid},
        {"amount", e.amount},
        {"comment", e.comment},
    };
    return {
        {"batch_id", e.batch_id},
        // Имя устройства бота в wallet-api (device.name) — для людей.
        {"device_name", "Telegram"},
        {"changes", {{"operation", json::array({op})}}},
    };
}

PushResult parse_push_response(int http_status, const std::string& body) {
    using S = PushResult::Status;
    PushResult out;
    out.http_status = http_status;
    json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_object()) {
        auto err = parsed.find("error");
        if (err != parsed.end() && err->is_string()) out.error = err->get<std::string>();
    }

    if (http_status == 200) {
        auto seq = parsed.is_object() ? parsed.find("seq") : parsed.end();
        if (!parsed.is_object() || seq == parsed.end() || !seq->is_number_integer()) {
            // Записано или нет — неизвестно; повтор тем же batch_id покажет.
            out.status = S::Unavailable;
            out.error = "ответ 200 без seq";
            return out;
        }
        out.status = S::Ok;
        out.seq = seq->get<std::int64_t>();
        return out;
    }
    if (http_status == 401) {
        out.status = S::Unauthorized;
    } else if (http_status == 403 && out.error == "operation_exists") {
        out.status = S::AlreadyExists;
    } else if (http_status == 403 && out.error == "reader_cannot_write") {
        out.status = S::Reader;
    } else if (http_status == 409 && out.error == "missing_reference") {
        out.status = S::MissingReference;
    } else if (http_status == 400 || http_status == 403 || http_status == 409) {
        out.status = S::Rejected;
        if (out.error.empty()) out.error = "HTTP " + std::to_string(http_status);
    } else {
        out.status = S::Unavailable;
        if (out.error.empty()) out.error = "HTTP " + std::to_string(http_status);
    }
    return out;
}

const char* status_name(PushResult::Status status) {
    using S = PushResult::Status;
    switch (status) {
    case S::Ok: return "ok";
    case S::AlreadyExists: return "already_exists";
    case S::Unauthorized: return "unauthorized";
    case S::Reader: return "reader";
    case S::MissingReference: return "missing_reference";
    case S::Rejected: return "rejected";
    case S::Unavailable: return "unavailable";
    }
    return "unknown";
}

std::unique_ptr<WalletApi> make_http_api(const std::string& base_url) {
    return std::make_unique<HttpWalletApi>(base_url);
}

} // namespace wallet
