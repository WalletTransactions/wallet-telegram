// Клиент служебных ручек google-auth для бота (org/docs/TODO_telegram_bot.md,
// §1.2; org/docs/TODO_telegram_tokens.md, шаг 6; сторона сервера —
// google-auth/src/telegram.hpp). Ходит напрямую на google-auth по внутренней
// сети, НЕ через wallet-gateway: тот /internal/* не маршрутизирует.
//
// Доступ бота к кошельку — связка tg_user_id → user_id в google-auth.
// Токенов пользователя бот не хранит: на каждую команду берёт короткий
// (5 мин) токен со scope reports:read expense:write.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace auth {

struct ExchangeResult {
    enum class Status {
        Ok,
        CodeRejected, // 410: код истёк, уже использован или пользователя нет
        BadRequest,   // 400: код или tg_user_id не прошли проверку
        Unavailable,  // сеть, 5xx, 401 (неверный секрет) и всё прочее
    };
    Status status = Status::Unavailable;
    std::string user_id;
    std::string device_id;
    std::string error; // для лога; токенов в нём нет
};

// POST /internal/telegram/token {tg_user_id}.
struct TokenResult {
    enum class Status {
        Ok,
        NotLinked,   // 404: связки нет — отключили из веба или не подключали
        Unavailable, // сеть, 5xx, 401 (неверный секрет) — связку не трогать
    };
    Status status = Status::Unavailable;
    std::string access_token;
    int expires_in = 0;
    std::string error;
};

class AuthApi {
public:
    virtual ~AuthApi() = default;
    virtual ExchangeResult exchange(const std::string& code, std::int64_t tg_user_id,
                                    const std::string& tg_username) = 0;
    virtual TokenResult token(std::int64_t tg_user_id) = 0;
    // DELETE /internal/telegram/links/{tg_user_id} — /unlink. true — связки
    // больше нет (удалили или её и не было, 404); false — не получилось
    // (сеть, 5xx).
    virtual bool unlink(std::int64_t tg_user_id) = 0;
};

// base_url — AUTH_SERVICE_URL (http://google-auth:8080), service_secret —
// TELEGRAM_SERVICE_SECRET, тот же, что в google-auth/.env.
std::unique_ptr<AuthApi> make_http_api(const std::string& base_url, const std::string& service_secret);

} // namespace auth
