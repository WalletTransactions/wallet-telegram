// Клиент wallet-api (org/docs/TODO_telegram_bot.md, «Подшаг 4 подробно»):
// запись траты — POST /sync/push с одной новой операцией, от имени
// пользователя токеном бота (scope expense:write). wallet-api пускает с ним
// только новые операции (PushMode::CreateOperationsOnly), повтор того же
// batch_id отвечает сохранённым результатом.
//
// Сам клиент ничего не повторяет: таймаут не значит «не записано», а повтор
// тем же batch_id безопасен — его делает пользователь кнопкой.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "nlohmann/json.hpp"

namespace wallet {

struct Expense {
    std::string batch_id;      // v5 от draft_id — повтор даёт тот же батч
    std::string op_uuid;       // тоже v5 от draft_id, другое имя
    std::string date;          // "YYYY-MM-DD HH:MM:SS", timestamp без TZ (parser::ParsedExpense::datetime)
    std::string wallet_uuid;   // acc_fr_uuid: расход — из кошелька
    std::string category_uuid; // acc_to_uuid
    std::string amount;        // десятичная строка, "350.5" — numeric на сервере, без double
    std::string comment;
};

struct PushResult {
    enum class Status {
        Ok,               // 200: записано (или повтор того же batch_id)
        AlreadyExists,    // 403 operation_exists: этот op_uuid уже записан — для бота тоже «записано»
        Unauthorized,     // 401: повторить с новым токеном
        Reader,           // 403 reader_cannot_write
        MissingReference, // 409 missing_reference: счёт исчез между сверкой и записью
        Rejected,         // 400, прочие 403/409: ошибка протокола — баг, в лог error
        Unavailable,      // сеть, таймаут, 5xx, непонятный ответ: можно нажать ещё раз
    };
    Status status = Status::Unavailable;
    int http_status = 0; // 0 — до HTTP-ответа не дошло
    std::int64_t seq = 0;
    std::string error; // код из тела ответа ("operation_exists", ...) или описание сбоя — для лога
};

class WalletApi {
public:
    virtual ~WalletApi() = default;
    virtual PushResult push_expense(const Expense& e, const std::string& access_token) = 0;
};

// Тело POST /sync/push — чистая функция ради тестов: в changes ровно одна
// таблица operation, поля deleted нет вовсе.
nlohmann::json push_body(const Expense& e);

// Ответ wallet-api → PushResult (таблица в плане) — чистая функция.
PushResult parse_push_response(int http_status, const std::string& body);

// Для логов и метрик: "ok", "already_exists", ...
const char* status_name(PushResult::Status status);

std::unique_ptr<WalletApi> make_http_api(const std::string& base_url);

} // namespace wallet
