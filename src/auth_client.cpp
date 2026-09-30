#include "auth_client.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace auth {

namespace {

class HttpAuthApi final : public AuthApi {
public:
    HttpAuthApi(const std::string& base_url, std::string service_secret)
        : base_url_(base_url), secret_(std::move(service_secret)) {}

    ExchangeResult exchange(const std::string& code, std::int64_t tg_user_id,
                            const std::string& tg_username) override {
        auto client = make_client();
        json body = {{"code", code}, {"tg_user_id", tg_user_id}, {"tg_username", tg_username}};
        auto res = client.Post("/internal/telegram/exchange", headers(), body.dump(), "application/json");

        ExchangeResult out;
        if (!res) {
            out.error = "google-auth недоступен: " + httplib::to_string(res.error());
            return out;
        }
        json parsed = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
        if (res->status == 200 && parsed.is_object()) {
            out.user_id = parsed.value("user_id", std::string{});
            out.device_id = parsed.value("device_id", std::string{});
            if (out.user_id.empty() || out.device_id.empty()) {
                out.error = "ответ google-auth без user_id/device_id";
                return out;
            }
            out.status = ExchangeResult::Status::Ok;
            return out;
        }
        out.error = "HTTP " + std::to_string(res->status) + ": " + error_of(parsed);
        if (res->status == 410) out.status = ExchangeResult::Status::CodeRejected;
        else if (res->status == 400) out.status = ExchangeResult::Status::BadRequest;
        return out;
    }

    TokenResult token(std::int64_t tg_user_id) override {
        auto client = make_client();
        json body = {{"tg_user_id", tg_user_id}};
        auto res = client.Post("/internal/telegram/token", headers(), body.dump(), "application/json");

        TokenResult out;
        if (!res) {
            out.error = "google-auth недоступен: " + httplib::to_string(res.error());
            return out;
        }
        json parsed = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
        if (res->status == 404) {
            out.status = TokenResult::Status::NotLinked;
            return out;
        }
        if (res->status != 200 || !parsed.is_object()) {
            out.error = "HTTP " + std::to_string(res->status) + ": " + error_of(parsed);
            return out;
        }
        out.access_token = parsed.value("access_token", std::string{});
        out.expires_in = parsed.value("expires_in", 0);
        if (out.access_token.empty()) {
            out.error = "ответ /internal/telegram/token без access_token";
            return out;
        }
        out.status = TokenResult::Status::Ok;
        return out;
    }

    bool unlink(std::int64_t tg_user_id) override {
        auto client = make_client();
        auto res = client.Delete("/internal/telegram/links/" + std::to_string(tg_user_id), headers());
        return res && (res->status == 204 || res->status == 404);
    }

private:
    httplib::Client make_client() const {
        // Клиент на вызов: запросы редкие (по одному на команду), держать
        // соединение ради них незачем.
        httplib::Client client(base_url_);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);
        return client;
    }

    httplib::Headers headers() const {
        httplib::Headers h = {{"X-Service-Secret", secret_}};
        // trace_id апдейта — чтобы запрос бота нашёлся в логах google-auth.
        std::string trace_id = applog::current_trace_id();
        if (!trace_id.empty()) h.emplace("X-Trace-Id", trace_id);
        return h;
    }

    static std::string error_of(const json& parsed) {
        return parsed.is_object() ? parsed.value("error", std::string{}) : std::string{};
    }

    std::string base_url_;
    std::string secret_;
};

} // namespace

std::unique_ptr<AuthApi> make_http_api(const std::string& base_url, const std::string& service_secret) {
    return std::make_unique<HttpAuthApi>(base_url, service_secret);
}

} // namespace auth
