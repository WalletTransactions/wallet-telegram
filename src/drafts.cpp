#include "drafts.hpp"

namespace drafts {

Draft& Drafts::put(Draft draft, std::time_t now) {
    for (auto it = by_id_.begin(); it != by_id_.end();) {
        if (now - it->second.created_at >= kTtlSecs) it = by_id_.erase(it);
        else ++it;
    }
    by_id_.erase(draft.id);

    // Лимит на пользователя — чтобы поток трат без нажатий не рос в памяти.
    std::size_t mine = 0;
    auto oldest = by_id_.end();
    for (auto it = by_id_.begin(); it != by_id_.end(); ++it) {
        if (it->second.tg_user_id != draft.tg_user_id) continue;
        ++mine;
        if (oldest == by_id_.end() || it->second.created_at < oldest->second.created_at) oldest = it;
    }
    if (mine >= kMaxPerUser && oldest != by_id_.end()) by_id_.erase(oldest);

    std::string id = draft.id;
    return by_id_[id] = std::move(draft);
}

Draft* Drafts::find(const std::string& id, std::time_t now) {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return nullptr;
    if (now - it->second.created_at >= kTtlSecs) {
        by_id_.erase(it);
        return nullptr;
    }
    return &it->second;
}

void Drafts::erase(const std::string& id) { by_id_.erase(id); }

} // namespace drafts
