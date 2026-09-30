#include "link_repo.hpp"

namespace links {

void PgLinkStore::save_link(const Link& link) {
    // Пустой default_wallet_uuid — NULL.
    db_.exec(
        "INSERT INTO tg_link (tg_user_id, chat_id, user_id, device_id, tz, default_wallet_uuid) "
        "VALUES ($1, $2, $3, $4, $5, NULLIF($6, '')::uuid) "
        "ON CONFLICT (tg_user_id) DO UPDATE SET "
        "chat_id = EXCLUDED.chat_id, user_id = EXCLUDED.user_id, device_id = EXCLUDED.device_id, "
        "tz = EXCLUDED.tz, default_wallet_uuid = EXCLUDED.default_wallet_uuid, last_used_at = now()",
        {std::to_string(link.tg_user_id), std::to_string(link.chat_id), link.user_id, link.device_id, link.tz,
         link.default_wallet_uuid});
}

std::optional<Link> PgLinkStore::find_link(std::int64_t tg_user_id) {
    auto res = db_.exec(
        "SELECT tg_user_id, chat_id, user_id, device_id, tz, COALESCE(default_wallet_uuid::text, '') "
        "FROM tg_link WHERE tg_user_id = $1",
        {std::to_string(tg_user_id)});
    if (res.rows.empty()) return std::nullopt;
    const auto& f = res.rows[0].fields;
    Link link;
    link.tg_user_id = std::stoll(f.at(0));
    link.chat_id = std::stoll(f.at(1));
    link.user_id = f.at(2);
    link.device_id = f.at(3);
    link.tz = f.at(4);
    link.default_wallet_uuid = f.at(5);
    return link;
}

void PgLinkStore::set_default_wallet(std::int64_t tg_user_id, const std::string& wallet_uuid) {
    db_.exec("UPDATE tg_link SET default_wallet_uuid = NULLIF($2, '')::uuid WHERE tg_user_id = $1",
             {std::to_string(tg_user_id), wallet_uuid});
}

void PgLinkStore::set_tz(std::int64_t tg_user_id, const std::string& tz) {
    db_.exec("UPDATE tg_link SET tz = $2 WHERE tg_user_id = $1", {std::to_string(tg_user_id), tz});
}

void PgLinkStore::delete_link(std::int64_t tg_user_id) {
    db_.exec("DELETE FROM tg_link WHERE tg_user_id = $1", {std::to_string(tg_user_id)});
}

std::int64_t PgLinkStore::load_offset() {
    auto res = db_.exec("SELECT next_update_id FROM tg_update_offset WHERE id = 1");
    if (res.rows.empty()) return 0;
    return std::stoll(res.rows[0].fields.at(0));
}

void PgLinkStore::save_offset(std::int64_t offset) {
    db_.exec(
        "INSERT INTO tg_update_offset (id, next_update_id) VALUES (1, $1) "
        "ON CONFLICT (id) DO UPDATE SET next_update_id = EXCLUDED.next_update_id, updated_at = now()",
        {std::to_string(offset)});
}

} // namespace links
