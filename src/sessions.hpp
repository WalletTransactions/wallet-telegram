// Токены бота для походов в wallet-report / wallet-api
// (org/docs/TODO_telegram_tokens.md, шаг 6).
//
// Бот не хранит токенов пользователя: на команду берёт у google-auth короткий
// токен по tg_user_id (POST /internal/telegram/token, 5 минут, scope
// reports:read expense:write) и держит его в памяти до минуты до истечения.
// Никакой ротации, шифрования и записи в базу.
//
// Связку удалили (веб «Отключить», пользователь удалён) — google-auth
// отвечает 404: локальные настройки (tg_link) удаляются, пользователя просят
// подключиться заново.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "auth_client.hpp"
#include "link_repo.hpp"

namespace sessions {

struct Access {
    enum class Status {
        Ok,
        Revoked,     // связки больше нет — локальные настройки уже удалены
        Unavailable, // google-auth недоступен — ничего не трогаем
    };
    Status status = Status::Unavailable;
    std::string token;
};

class Sessions {
public:
    using Clock = std::chrono::steady_clock;

    Sessions(auth::AuthApi& auth, links::LinkStore& store) : auth_(auth), store_(store) {}

    // force_refresh — токен из кеша получил 401 (сменили ключи и т.п.): не
    // доверять кешу, взять новый.
    Access access_token(std::int64_t tg_user_id, bool force_refresh = false);
    void forget(std::int64_t tg_user_id);

private:
    struct Cached {
        std::string token;
        Clock::time_point expires_at;
    };

    auth::AuthApi& auth_;
    links::LinkStore& store_;
    std::unordered_map<std::int64_t, Cached> cache_;
};

} // namespace sessions
