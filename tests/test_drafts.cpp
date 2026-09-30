// Хранилище черновиков (drafts.hpp): TTL, лимит на пользователя, замена по id.
#include "drafts.hpp"

#include "test_framework.hpp"

namespace {

drafts::Draft draft(const std::string& id, std::int64_t user, std::time_t created) {
    drafts::Draft d;
    d.id = id;
    d.tg_user_id = user;
    d.created_at = created;
    return d;
}

} // namespace

TEST(drafts_ttl) {
    drafts::Drafts store;
    store.put(draft("1", 7, 1000), 1000);
    CHECK(store.find("1", 1000 + drafts::kTtlSecs - 1) != nullptr);
    CHECK(store.find("1", 1000 + drafts::kTtlSecs) == nullptr);
    CHECK_EQ(store.size(), 0u); // истёкший удалён при поиске
    CHECK(store.find("nope", 1000) == nullptr);
}

TEST(drafts_same_id_replaces) {
    drafts::Drafts store;
    store.put(draft("1", 7, 1000), 1000);
    drafts::Draft again = draft("1", 7, 1005);
    again.message_id = 55;
    store.put(std::move(again), 1005);
    CHECK_EQ(store.size(), 1u);
    REQUIRE(store.find("1", 1005) != nullptr);
    CHECK_EQ(store.find("1", 1005)->message_id, static_cast<std::int64_t>(55));
    store.erase("1");
    CHECK(store.find("1", 1005) == nullptr);
}

TEST(drafts_limit_per_user_evicts_oldest) {
    drafts::Drafts store;
    for (std::size_t i = 0; i < drafts::kMaxPerUser; ++i) {
        store.put(draft(std::to_string(i), 7, 1000 + static_cast<std::time_t>(i)), 1000);
    }
    store.put(draft("other", 8, 900), 1000); // чужие не считаются и не вытесняются
    store.put(draft("new", 7, 1100), 1100);
    CHECK(store.find("0", 1100) == nullptr);
    CHECK(store.find("1", 1100) != nullptr);
    CHECK(store.find("new", 1100) != nullptr);
    CHECK(store.find("other", 1100) != nullptr);
    CHECK_EQ(store.size(), drafts::kMaxPerUser + 1);
}

TEST(drafts_put_sweeps_expired) {
    drafts::Drafts store;
    store.put(draft("old", 7, 0), 0);
    store.put(draft("fresh", 8, drafts::kTtlSecs), drafts::kTtlSecs);
    CHECK_EQ(store.size(), 1u);
}
