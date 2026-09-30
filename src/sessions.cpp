#include "sessions.hpp"

#include "util/log.hpp"
#include "util/metrics.hpp"

namespace sessions {

namespace {

// Берём новый заранее: токен, истекающий через секунды, может умереть
// посреди похода в wallet-report.
constexpr auto kRefreshMargin = std::chrono::seconds(60);

void count_issue(const std::string& result) {
    metrics::inc_counter("tg_token_issue_total", "Выпуск токена бота у google-auth по результату.", "result",
                         result);
}

} // namespace

void Sessions::forget(std::int64_t tg_user_id) { cache_.erase(tg_user_id); }

Access Sessions::access_token(std::int64_t tg_user_id, bool force_refresh) {
    Access out;
    auto it = cache_.find(tg_user_id);
    if (!force_refresh && it != cache_.end() && it->second.expires_at - kRefreshMargin > Clock::now()) {
        out.status = Access::Status::Ok;
        out.token = it->second.token;
        return out;
    }

    auth::TokenResult r = auth_.token(tg_user_id);
    switch (r.status) {
    case auth::TokenResult::Status::Ok:
        break;
    case auth::TokenResult::Status::NotLinked:
        applog::info("telegram link gone in google-auth, removing local settings",
                     {{"event", "tg_session_revoked"}, {"tg_user_id", tg_user_id}});
        count_issue("not_linked");
        store_.delete_link(tg_user_id);
        forget(tg_user_id);
        out.status = Access::Status::Revoked;
        return out;
    case auth::TokenResult::Status::Unavailable:
        applog::warn("telegram token issue failed",
                     {{"event", "tg_token_issue_failed"}, {"tg_user_id", tg_user_id}, {"error", r.error}});
        count_issue("unavailable");
        return out;
    }

    int ttl = r.expires_in > 0 ? r.expires_in : 300;
    cache_[tg_user_id] = Cached{r.access_token, Clock::now() + std::chrono::seconds(ttl)};
    count_issue("ok");
    out.status = Access::Status::Ok;
    out.token = r.access_token;
    return out;
}

} // namespace sessions
