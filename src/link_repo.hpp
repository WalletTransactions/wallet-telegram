// Своя база бота (org/docs/TODO_telegram_bot.md, §1.4): настройки
// Telegram-пользователя (чат, пояс, кошелёк по умолчанию) и offset поллинга.
// Сама связка tg_user_id → user_id — в google-auth (telegram_links), здесь
// её копия для настроек; секретов в этой базе нет (шаг 6
// TODO_telegram_tokens.md). Схема — wallet-telegram/migrations/, роль
// бота — только DML (init-db.sh).
//
// LinkStore — интерфейс ради юнит-тестов Bot (фейк в памяти), реализация на
// Postgres — PgLinkStore ниже.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

namespace links {

struct Link {
    std::int64_t tg_user_id = 0;
    std::int64_t chat_id = 0;
    std::string user_id;
    std::string device_id;
    std::string tz; // IANA, например "Europe/Moscow"
    std::string default_wallet_uuid; // пусто — не выбран
};

class LinkStore {
public:
    virtual ~LinkStore() = default;

    // Вставка или замена связки этого tg_user_id целиком (повторный /start с
    // новым кодом). Что сохранить из прежней связки (tz, кошелёк по
    // умолчанию), решает вызывающий — Bot::handle_start.
    virtual void save_link(const Link& link) = 0;
    virtual std::optional<Link> find_link(std::int64_t tg_user_id) = 0;

    virtual void set_default_wallet(std::int64_t tg_user_id, const std::string& wallet_uuid) = 0;
    virtual void set_tz(std::int64_t tg_user_id, const std::string& tz) = 0;
    virtual void delete_link(std::int64_t tg_user_id) = 0;

    // update_id первого необработанного апдейта; 0 — поллинг ещё ни разу не
    // шёл (Telegram тогда отдаёт всё, что накопилось за последние 24 ч).
    virtual std::int64_t load_offset() = 0;
    virtual void save_offset(std::int64_t offset) = 0;
};

class PgLinkStore final : public LinkStore {
public:
    explicit PgLinkStore(db::Database& db) : db_(db) {}

    void save_link(const Link& link) override;
    std::optional<Link> find_link(std::int64_t tg_user_id) override;
    void set_default_wallet(std::int64_t tg_user_id, const std::string& wallet_uuid) override;
    void set_tz(std::int64_t tg_user_id, const std::string& tz) override;
    void delete_link(std::int64_t tg_user_id) override;
    std::int64_t load_offset() override;
    void save_offset(std::int64_t offset) override;

private:
    db::Database& db_;
};

} // namespace links
