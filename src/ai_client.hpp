// Клиент wallet-ai (org/docs/TODO_wallet_ai.md, §3; шаг 4 там): бот разбирает
// трату свободным текстом асинхронным заданием parse_expense. Ходит напрямую
// на wallet-ai по внутренней сети токеном пользователя (aud wallet-ai, scope
// ai:use — их выдаёт google-auth токену бота).
//
// Задание создаётся в потоке поллинга, результат забирается там же коротким
// опросом (Bot::ai_tick): HTTP-клиент Telegram, соединение с базой и
// черновики бота однопоточны.
#pragma once

#include "net/tls.hpp"

#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace ai {

struct CreateResult {
    enum class Status {
        Created,
        Unavailable,         // 503: ИИ выключен или LLM не отвечает
        ProviderUnavailable, // 503 provider_unavailable (chat): выбранный провайдер недоступен
        Busy,                // 429: очередь полна, лимиты пользователя
        Unauthorized, // 401: токен не принят (повтор с новым)
        Error,        // сеть, 4xx/5xx — ИИ как будто нет
    };
    Status status = Status::Error;
    std::string job_id;
    std::string error; // для лога
};

struct JobResult {
    enum class Status {
        Pending,      // queued / running
        Done,         // result — ParseExpenseResult (outcome draft / not_expense / rejected)
        Failed,       // failure: timeout, unavailable, bad_response, internal
        Cancelled,
        NotFound,     // 404: истекло или wallet-ai перезапускался
        Unauthorized, // 401
        Error,        // сеть, 5xx — попробовать ещё раз
    };
    Status status = Status::Error;
    nlohmann::json result;
    std::string failure;
    std::string error;
};

// Событие хода задания chat (GET /api/ai/jobs/{id}/events, change
// chat-live-reply): delta {text}, tool {name, outcome}, done {result},
// error {failure}.
struct StreamEvent {
    std::int64_t id = 0;
    std::string type;
    nlohmann::json data;
};

// Что вернуло короткое окно чтения событий.
struct EventsResult {
    enum class Status {
        Ok,           // 200: events — пришедшее за окно (возможно, пусто)
        Disabled,     // 404: потока нет (выключен, задание истекло или не chat)
        Busy,         // 429: слишком много потоков
        Unauthorized, // 401
        Error,        // сеть, 5xx
    };
    Status status = Status::Error;
    std::vector<StreamEvent> events;
    std::string error;
};

// Настройки ИИ пользователя (GET/PUT /api/ai/settings, схема AiSettings
// wallet-ai): выбранный провайдер чата и согласие на облачного.
struct SettingsResult {
    enum class Status {
        Ok,                  // 200, settings — AiSettings
        ProviderUnavailable, // 400 provider_unavailable: неизвестный или недоступный провайдер
        ConsentRequired,     // 409 consent_required: облачный провайдер без согласия
        Unauthorized,        // 401 (повтор с новым токеном)
        Unavailable,         // 503, сеть, прочие ответы — «сервис недоступен»
    };
    Status status = Status::Unavailable;
    nlohmann::json settings;
    std::string error; // для лога
};

const char* to_string(CreateResult::Status s);
const char* to_string(JobResult::Status s);
const char* to_string(SettingsResult::Status s);
const char* to_string(EventsResult::Status s);

// Разбор ответов — чистые функции ради юнит-тестов.
CreateResult parse_create_response(int http_status, const std::string& body);
JobResult parse_job_response(int http_status, const std::string& body);
SettingsResult parse_settings_response(int http_status, const std::string& body);
// Кадры text/event-stream: только целые (до пустой строки), оборванный хвост
// отбрасывается; пульс (`: ping`) и неизвестные поля пропускаются.
std::vector<StreamEvent> parse_events_body(const std::string& body);
EventsResult parse_events_response(int http_status, const std::string& body);

class AiApi {
public:
    virtual ~AiApi() = default;
    // POST /api/ai/jobs {kind: parse_expense, input, idempotency_key}.
    virtual CreateResult create_parse_expense(const std::string& token, const nlohmann::json& input,
                                              const std::string& idempotency_key) = 0;
    // POST /api/ai/jobs {kind: chat, input, idempotency_key}; input — text,
    // conversation_id, now.
    virtual CreateResult create_chat(const std::string& token, const nlohmann::json& input,
                                     const std::string& idempotency_key) = 0;
    // GET /api/ai/settings.
    virtual SettingsResult get_settings(const std::string& token) = 0;
    // PUT /api/ai/settings {provider, consent?}.
    virtual SettingsResult put_settings(const std::string& token, const nlohmann::json& body) = 0;
    // GET /api/ai/jobs/{id} без wait: бот опрашивает сам, не держа поток.
    virtual JobResult get_job(const std::string& token, const std::string& job_id) = 0;
    // DELETE /api/ai/jobs/{id}; ошибки глотаются — задание и так истечёт.
    virtual void cancel_job(const std::string& token, const std::string& job_id) = 0;
    // GET /api/ai/jobs/{id}/events с Last-Event-ID = after_id: слушает поток
    // не дольше window_ms и возвращает пришедшее. Бот однопоточный, держать
    // соединение дольше окна нельзя — продолжает со следующего тика. Умолчание
    // — «потока нет», тогда бот опрашивает статус как раньше.
    virtual EventsResult get_events(const std::string& token, const std::string& job_id, std::int64_t after_id,
                                    int window_ms) {
        (void)token; (void)job_id; (void)after_id; (void)window_ms;
        EventsResult r;
        r.status = EventsResult::Status::Disabled;
        return r;
    }
    // GET /api/ai/status -> enabled; nullopt — не удалось узнать.
    virtual std::optional<bool> enabled(const std::string& token) = 0;
};

std::unique_ptr<AiApi> make_http_api(const std::string& base_url, const nettls::Config& tls = {});

} // namespace ai
