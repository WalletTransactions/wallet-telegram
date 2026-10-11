#include "telegram_client.hpp"

#include <chrono>

#include "httplib.h"
#include "expense/text.hpp"
#include "util/metrics.hpp"

using nlohmann::json;

namespace telegram {

namespace {

std::int64_t int64_or_zero(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return 0;
    return it->get<std::int64_t>();
}

std::string string_or_empty(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

void count_api_error(const std::string& method) {
    metrics::inc_counter("tg_api_errors_total", "Ошибки вызовов Telegram Bot API по методу.", "method",
                         method);
}

class HttpTelegramApi final : public TelegramApi {
public:
    HttpTelegramApi(const std::string& base_url, std::string bot_token)
        : client_(base_url), token_(std::move(bot_token)) {
        client_.set_connection_timeout(10, 0);
        // Длинный опрос держит запрос до timeout_secs — таймаут чтения
        // выставляется в get_updates под конкретный вызов.
        client_.set_read_timeout(15, 0);
        client_.set_write_timeout(10, 0);
    }

    std::vector<Update> get_updates(std::int64_t offset, int timeout_secs) override {
        json body = {
            {"offset", offset},
            {"timeout", timeout_secs},
            // Текстовые сообщения и нажатия inline-кнопок (карточка траты).
            {"allowed_updates", json::array({"message", "callback_query"})},
        };
        client_.set_read_timeout(timeout_secs + 10, 0);
        json result = call("getUpdates", body);
        client_.set_read_timeout(15, 0);
        return parse_updates(result);
    }

    std::int64_t send_message(std::int64_t chat_id, const std::string& text, const Keyboard& keyboard) override {
        json payload = {{"chat_id", chat_id}, {"text", text}};
        if (!keyboard.empty()) payload["reply_markup"] = keyboard_json(keyboard);
        json result = call("sendMessage", payload);
        auto msg = result.find("result");
        return msg != result.end() && msg->is_object() ? int64_or_zero(*msg, "message_id") : 0;
    }

    void edit_message(std::int64_t chat_id, std::int64_t message_id, const std::string& text,
                      const Keyboard& keyboard) override {
        // Пустая inline_keyboard явно — клавиатура убирается.
        json payload = {{"chat_id", chat_id},
                        {"message_id", message_id},
                        {"text", text},
                        {"reply_markup", keyboard_json(keyboard)}};
        try {
            call("editMessageText", payload);
        } catch (const ApiError& e) {
            // Двойной клик той же кнопки: новый текст и клавиатура совпали со
            // старыми. Сообщение и так в нужном виде.
            if (e.http_status == 400 && std::string(e.what()).find("message is not modified") != std::string::npos) {
                return;
            }
            throw;
        }
    }

    void answer_callback(const std::string& callback_id, const std::string& text, bool alert) override {
        json payload = {{"callback_query_id", callback_id}};
        if (!text.empty()) {
            payload["text"] = text;
            payload["show_alert"] = alert;
        }
        call("answerCallbackQuery", payload);
    }

private:
    json call(const std::string& method, const json& payload) {
        auto started = std::chrono::steady_clock::now();
        auto res = client_.Post("/bot" + token_ + "/" + method, payload.dump(), "application/json");
        // getUpdates — длинный опрос, его длительность — это timeout, а не
        // скорость Telegram: в гистограмму не идёт.
        if (method != "getUpdates") {
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            metrics::observe_duration("tg_telegram_api_duration_seconds",
                                      "Длительность вызовов Telegram Bot API (кроме getUpdates).", "method", method,
                                      seconds);
        }
        if (!res) {
            count_api_error(method);
            throw ApiError(method, 0, httplib::to_string(res.error()));
        }
        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception&) {
            count_api_error(method);
            throw ApiError(method, res->status, "ответ не JSON");
        }
        if (res->status != 200 || !parsed.value("ok", false)) {
            count_api_error(method);
            throw ApiError(method, res->status, parsed.value("description", std::string("ok=false")));
        }
        return parsed;
    }

    httplib::Client client_;
    std::string token_;
};

void read_chat_and_from(const json& msg, Update& u) {
    auto chat = msg.find("chat");
    if (chat != msg.end() && chat->is_object()) {
        u.chat_id = int64_or_zero(*chat, "id");
        u.chat_type = string_or_empty(*chat, "type");
    }
    auto from = msg.find("from");
    if (from != msg.end() && from->is_object()) {
        u.from_id = int64_or_zero(*from, "id");
        u.from_username = string_or_empty(*from, "username");
    }
}

} // namespace

json keyboard_json(const Keyboard& keyboard) {
    json rows = json::array();
    for (const auto& row : keyboard) {
        json buttons = json::array();
        for (const auto& b : row) {
            if (b.data.empty() || b.data.size() > kMaxCallbackDataBytes) {
                throw std::invalid_argument("callback_data: " + std::to_string(b.data.size()) + " байт, нужно 1.." +
                                            std::to_string(kMaxCallbackDataBytes));
            }
            buttons.push_back({{"text", text::truncate_utf8(b.text, kMaxButtonTextBytes)}, {"callback_data", b.data}});
        }
        rows.push_back(std::move(buttons));
    }
    return {{"inline_keyboard", std::move(rows)}};
}

std::vector<Update> parse_updates(const json& body) {
    if (!body.is_object() || !body.value("ok", false)) {
        throw ApiError("getUpdates", 200, "ok != true");
    }
    auto it = body.find("result");
    if (it == body.end() || !it->is_array()) {
        throw ApiError("getUpdates", 200, "result не массив");
    }

    std::vector<Update> out;
    for (const auto& item : *it) {
        if (!item.is_object()) continue;
        Update u;
        u.update_id = int64_or_zero(item, "update_id");
        auto msg = item.find("message");
        auto cb = item.find("callback_query");
        if (msg != item.end() && msg->is_object()) {
            u.has_message = true;
            u.text = string_or_empty(*msg, "text");
            u.message_id = int64_or_zero(*msg, "message_id");
            read_chat_and_from(*msg, u);
        } else if (cb != item.end() && cb->is_object()) {
            u.has_callback = true;
            u.callback_id = string_or_empty(*cb, "id");
            u.callback_data = string_or_empty(*cb, "data");
            // Чат и сообщение — из message, где кнопка; нажал — callback.from
            // (from у message — сам бот).
            auto cb_msg = cb->find("message");
            if (cb_msg != cb->end() && cb_msg->is_object()) {
                u.message_id = int64_or_zero(*cb_msg, "message_id");
                read_chat_and_from(*cb_msg, u);
            }
            u.from_id = 0;
            u.from_username.clear();
            auto from = cb->find("from");
            if (from != cb->end() && from->is_object()) {
                u.from_id = int64_or_zero(*from, "id");
                u.from_username = string_or_empty(*from, "username");
            }
            if (u.chat_id == 0) u.chat_id = u.from_id;
        }
        out.push_back(std::move(u));
    }
    return out;
}

std::unique_ptr<TelegramApi> make_http_api(const std::string& base_url, const std::string& bot_token) {
    return std::make_unique<HttpTelegramApi>(base_url, bot_token);
}

} // namespace telegram
