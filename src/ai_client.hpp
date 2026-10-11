// Клиент wallet-ai (org/docs/TODO_wallet_ai.md, §3; шаг 4 там): бот разбирает
// трату свободным текстом асинхронным заданием parse_expense. Ходит напрямую
// на wallet-ai по внутренней сети токеном пользователя (aud wallet-ai, scope
// ai:use — их выдаёт google-auth токену бота).
//
// Задание создаётся в потоке поллинга, результат забирается там же коротким
// опросом (Bot::ai_tick): HTTP-клиент Telegram, соединение с базой и
// черновики бота однопоточны.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace ai {

struct CreateResult {
    enum class Status {
        Created,
        Unavailable,  // 503: ИИ выключен или LLM не отвечает
        Busy,         // 429: очередь полна, лимиты пользователя
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

const char* to_string(CreateResult::Status s);
const char* to_string(JobResult::Status s);

// Разбор ответов — чистые функции ради юнит-тестов.
CreateResult parse_create_response(int http_status, const std::string& body);
JobResult parse_job_response(int http_status, const std::string& body);

class AiApi {
public:
    virtual ~AiApi() = default;
    // POST /api/ai/jobs {kind: parse_expense, input, idempotency_key}.
    virtual CreateResult create_parse_expense(const std::string& token, const nlohmann::json& input,
                                              const std::string& idempotency_key) = 0;
    // GET /api/ai/jobs/{id} без wait: бот опрашивает сам, не держа поток.
    virtual JobResult get_job(const std::string& token, const std::string& job_id) = 0;
    // DELETE /api/ai/jobs/{id}; ошибки глотаются — задание и так истечёт.
    virtual void cancel_job(const std::string& token, const std::string& job_id) = 0;
    // GET /api/ai/status -> enabled; nullopt — не удалось узнать.
    virtual std::optional<bool> enabled(const std::string& token) = 0;
};

std::unique_ptr<AiApi> make_http_api(const std::string& base_url);

} // namespace ai
