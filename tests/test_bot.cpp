// Bot::handle на фейках TelegramApi/AuthApi/ReportsApi/LinkStore — без сети
// и базы.
#include "bot.hpp"

#include <atomic>
#include <cstdio>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "card.hpp"
#include "test_framework.hpp"
#include "util/metrics.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace {

class FakeTelegram final : public telegram::TelegramApi {
public:
    std::vector<telegram::Update> get_updates(std::int64_t, int) override { return {}; }
    std::int64_t send_message(std::int64_t chat_id, const std::string& text,
                              const telegram::Keyboard& keyboard) override {
        sent.emplace_back(chat_id, text);
        keyboards.push_back(keyboard);
        return next_message_id++;
    }
    void edit_message(std::int64_t chat_id, std::int64_t message_id, const std::string& text,
                      const telegram::Keyboard& keyboard) override {
        edits.push_back({chat_id, message_id, text, keyboard});
    }
    void answer_callback(const std::string& callback_id, const std::string& text, bool alert) override {
        answers.push_back({callback_id, text, alert});
    }

    struct Edit {
        std::int64_t chat_id;
        std::int64_t message_id;
        std::string text;
        telegram::Keyboard keyboard;
    };
    struct Answer {
        std::string callback_id;
        std::string text;
        bool alert;
    };
    std::vector<std::pair<std::int64_t, std::string>> sent;
    std::vector<telegram::Keyboard> keyboards; // клавиатура каждого sent
    std::vector<Edit> edits;
    std::vector<Answer> answers;
    std::int64_t next_message_id = 100;
};

class FakeAuth final : public auth::AuthApi {
public:
    auth::ExchangeResult exchange(const std::string& code, std::int64_t tg_user_id,
                                  const std::string& tg_username) override {
        calls.push_back({code, tg_user_id, tg_username});
        return next;
    }
    // Каждый выпуск — новый токен: access-1, access-2, ...
    auth::TokenResult token(std::int64_t tg_user_id) override {
        token_calls.push_back(tg_user_id);
        auth::TokenResult r;
        r.status = token_status;
        if (r.status == auth::TokenResult::Status::Ok) {
            ++issued;
            r.access_token = "access-" + std::to_string(issued);
            r.expires_in = 300;
        }
        return r;
    }
    bool unlink(std::int64_t tg_user_id) override {
        unlinks.push_back(tg_user_id);
        return unlink_ok;
    }

    struct Call {
        std::string code;
        std::int64_t tg_user_id;
        std::string tg_username;
    };
    std::vector<Call> calls;
    auth::ExchangeResult next;
    auth::TokenResult::Status token_status = auth::TokenResult::Status::Ok;
    std::vector<std::int64_t> token_calls;
    std::vector<std::int64_t> unlinks;
    bool unlink_ok = true;
    int issued = 0;
};

// Ответы по префиксу пути; запоминает, с каким токеном и куда ходили.
class FakeReports final : public reports::ReportsApi {
public:
    reports::Response get(const std::string& path_and_query, const std::string& access_token) override {
        calls.emplace_back(path_and_query, access_token);
        if (unauthorized_tokens.count(access_token) != 0) return reports::Response{401, json::object(), "HTTP 401"};
        for (const auto& [prefix, body] : routes) {
            if (path_and_query.compare(0, prefix.size(), prefix) == 0) return reports::Response{200, body, ""};
        }
        return reports::Response{500, json::object(), "no route"};
    }

    std::vector<std::pair<std::string, json>> routes;
    std::unordered_set<std::string> unauthorized_tokens;
    std::vector<std::pair<std::string, std::string>> calls;
};

// Запоминает вызовы; на токен из unauthorized_tokens — 401, иначе next.
class FakeWallet final : public wallet::WalletApi {
public:
    wallet::PushResult push_expense(const wallet::Expense& e, const std::string& access_token) override {
        calls.emplace_back(e, access_token);
        if (unauthorized_tokens.count(access_token) != 0) {
            wallet::PushResult r;
            r.status = wallet::PushResult::Status::Unauthorized;
            r.http_status = 401;
            return r;
        }
        return next;
    }
    std::vector<std::pair<wallet::Expense, std::string>> calls;
    wallet::PushResult next;
    std::unordered_set<std::string> unauthorized_tokens;
};

class FakeStore final : public links::LinkStore {
public:
    void save_link(const links::Link& link) override { links[link.tg_user_id] = link; }
    std::optional<links::Link> find_link(std::int64_t id) override {
        auto it = links.find(id);
        if (it == links.end()) return std::nullopt;
        return it->second;
    }
    void set_default_wallet(std::int64_t id, const std::string& uuid) override {
        links.at(id).default_wallet_uuid = uuid;
    }
    void set_tz(std::int64_t id, const std::string& tz) override { links.at(id).tz = tz; }
    void delete_link(std::int64_t id) override { links.erase(id); }
    std::int64_t load_offset() override { return offset; }
    void save_offset(std::int64_t o) override { offset = o; }

    std::map<std::int64_t, links::Link> links;
    std::int64_t offset = 0;
};

constexpr std::int64_t kUser = 211959201;
const std::string kCode = "Ab-_09Ab-_09Ab-_09Ab-_09Ab-_09Ab";

telegram::Update private_message(const std::string& text, std::int64_t from = kUser) {
    telegram::Update u;
    u.update_id = 1;
    u.has_message = true;
    u.chat_id = from;
    u.chat_type = "private";
    u.from_id = from;
    u.from_username = "ivan";
    u.text = text;
    return u;
}

auth::ExchangeResult ok_exchange(const std::string& user_id = "user-1") {
    auth::ExchangeResult r;
    r.status = auth::ExchangeResult::Status::Ok;
    r.user_id = user_id;
    r.device_id = "device-1";
    return r;
}

struct Harness {
    FakeTelegram tg;
    FakeAuth auth;
    FakeReports reports;
    FakeWallet wallet;
    FakeStore store;
    bot::Config config;
    std::unique_ptr<bot::Bot> bot;
    std::time_t now = 1790000000; // 2026-09-21 ~14:13 UTC, в Москве 17:13

    Harness() {
        config.bot_username = "WalletTransaction_bot";
        config.default_tz = "Europe/Moscow";
        config.clock = [this] { return now; };
        rebuild();
    }
    void rebuild() { bot = std::make_unique<bot::Bot>(tg, auth, reports, wallet, store, config); }
    std::string last_reply() const { return tg.sent.empty() ? std::string{} : tg.sent.back().second; }

    // Настройки, как будто /start прошёл раньше (в прошлом запуске бота —
    // кеша токенов нет, первый же запрос к отчётам пойдёт за токеном).
    void link(const std::string& default_wallet = "") {
        links::Link l;
        l.tg_user_id = kUser;
        l.chat_id = kUser;
        l.user_id = "user-1";
        l.device_id = "device-1";
        l.tz = "Europe/Moscow";
        l.default_wallet_uuid = default_wallet;
        store.save_link(l);
    }
};

json wallets_json() {
    return json::parse(R"({"accounts": [
      {"uuid": "w-1", "name": "Тинькофф", "kind": "wallet", "system_kind": 0, "is_archived": false,
       "icon_sha256": "", "currency_uuid": "c-rub", "iso_code": "RUB", "symbol": "₽", "precision": "2"},
      {"uuid": "w-2", "name": "Revolut", "kind": "wallet", "system_kind": 0, "is_archived": false,
       "icon_sha256": "", "currency_uuid": "c-usd", "iso_code": "USD", "symbol": "$", "precision": "2"}
    ]})");
}

// Кошельки и категории одним ответом — как GET /api/reports/accounts без kind.
json accounts_json() {
    json a = wallets_json();
    for (const auto& [uuid, name, system_kind] : std::vector<std::tuple<std::string, std::string, int>>{
             {"c-1", "Кафе", 0}, {"c-2", "Кафе и рестораны", 0}, {"c-3", "Продукты", 0}, {"c-4", "Transfer", 1}}) {
        a["accounts"].push_back({{"uuid", uuid}, {"name", name}, {"kind", "category"}, {"system_kind", system_kind},
                                 {"iso_code", ""}, {"symbol", ""}, {"precision", ""}});
    }
    return a;
}

std::size_t count_calls(const FakeReports& r, const std::string& prefix) {
    std::size_t n = 0;
    for (const auto& c : r.calls) {
        if (c.first.compare(0, prefix.size(), prefix) == 0) ++n;
    }
    return n;
}

} // namespace

TEST(start_with_code_links_chat) {
    Harness h;
    h.auth.next = ok_exchange();
    h.bot->handle(private_message("/start " + kCode));

    REQUIRE(h.auth.calls.size() == 1u);
    CHECK_EQ(h.auth.calls[0].code, kCode);
    CHECK_EQ(h.auth.calls[0].tg_user_id, kUser);
    CHECK_EQ(h.auth.calls[0].tg_username, std::string("ivan"));

    auto link = h.store.find_link(kUser);
    REQUIRE(link.has_value());
    CHECK_EQ(link->user_id, std::string("user-1"));
    CHECK_EQ(link->device_id, std::string("device-1"));
    CHECK_EQ(link->chat_id, kUser);
    CHECK_EQ(link->tz, std::string("Europe/Moscow"));
    CHECK(h.auth.token_calls.empty()); // токен — только когда понадобится

    CHECK_EQ(h.last_reply(), std::string(bot::texts::kLinked));
}

TEST(relink_same_user_keeps_tz_and_default_wallet) {
    Harness h;
    links::Link old;
    old.tg_user_id = kUser;
    old.user_id = "user-1";
    old.tz = "Asia/Tbilisi";
    old.default_wallet_uuid = "wallet-uuid";
    h.store.save_link(old);

    h.auth.next = ok_exchange("user-1");
    h.bot->handle(private_message("/start " + kCode));
    auto link = h.store.find_link(kUser);
    REQUIRE(link.has_value());
    CHECK_EQ(link->tz, std::string("Asia/Tbilisi"));
    CHECK_EQ(link->default_wallet_uuid, std::string("wallet-uuid"));
}

TEST(relink_other_user_keeps_tz_drops_default_wallet_and_cached_token) {
    Harness h;
    h.link("wallet-uuid");
    h.store.set_tz(kUser, "Asia/Tbilisi");
    // Токен прежнего пользователя уже в кеше.
    h.reports.routes = {{"/api/reports/accounts", json::parse(R"({"accounts": []})")}};
    h.bot->handle(private_message("/categories"));
    REQUIRE(h.auth.token_calls.size() == 1u);

    h.auth.next = ok_exchange("user-2");
    h.bot->handle(private_message("/start " + kCode));
    auto link = h.store.find_link(kUser);
    REQUIRE(link.has_value());
    CHECK_EQ(link->user_id, std::string("user-2"));
    CHECK_EQ(link->tz, std::string("Asia/Tbilisi"));
    CHECK(link->default_wallet_uuid.empty());
    // Токен прежнего пользователя из кеша выброшен — следующая команда
    // берёт новый (уже для user-2).
    h.bot->handle(private_message("/categories"));
    CHECK_EQ(h.auth.token_calls.size(), 2u);
    // Прежнюю связку в google-auth заменил сам обмен кода — бот ничего не
    // отзывает сам.
    CHECK(h.auth.unlinks.empty());
}

TEST(start_with_rejected_code_does_not_link) {
    Harness h;
    h.auth.next.status = auth::ExchangeResult::Status::CodeRejected;
    h.bot->handle(private_message("/start " + kCode));
    CHECK(!h.store.find_link(kUser).has_value());
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kCodeRejected));
}

TEST(start_when_auth_unavailable_says_so) {
    Harness h;
    h.auth.next.status = auth::ExchangeResult::Status::Unavailable;
    h.bot->handle(private_message("/start " + kCode));
    CHECK(!h.store.find_link(kUser).has_value());
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnavailable));
}

TEST(start_with_garbage_code_does_not_call_auth) {
    Harness h;
    h.bot->handle(private_message("/start not a code"));
    CHECK(h.auth.calls.empty());
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kBadCode));
}

TEST(start_without_code_gives_instructions) {
    Harness h;
    h.bot->handle(private_message("/start"));
    CHECK(h.auth.calls.empty());
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kStartWithoutCode));
}

TEST(help_replies_with_help) {
    Harness h;
    h.bot->handle(private_message("/help"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kHelp));
}

TEST(group_chat_is_ignored_silently) {
    Harness h;
    auto u = private_message("/start " + kCode);
    u.chat_type = "group";
    u.chat_id = -100;
    h.bot->handle(u);
    CHECK(h.tg.sent.empty());
    CHECK(h.auth.calls.empty());
}

TEST(non_message_update_is_ignored) {
    Harness h;
    telegram::Update u;
    u.update_id = 5;
    h.bot->handle(u);
    CHECK(h.tg.sent.empty());
}

// Кнопок бот пока не шлёт, но на нажатие (старой кнопки, чужого клиента)
// отвечает — иначе у пользователя крутятся часы.
TEST(callback_is_always_answered) {
    Harness h;
    telegram::Update u;
    u.update_id = 6;
    u.has_callback = true;
    u.callback_id = "cb-1";
    u.callback_data = "123:x";
    u.chat_id = u.from_id = kUser;
    u.chat_type = "private";
    h.bot->handle(u);
    REQUIRE(h.tg.answers.size() == 1u);
    CHECK_EQ(h.tg.answers[0].callback_id, std::string("cb-1"));
    CHECK(h.tg.sent.empty());
    CHECK(h.tg.edits.empty());
}

TEST(allowlist_blocks_strangers_and_lets_owner_in) {
    Harness h;
    h.config.allowed_user_ids = {kUser};
    h.rebuild();

    h.bot->handle(private_message("/help", 42));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kClosedBeta));

    h.auth.next = ok_exchange();
    h.bot->handle(private_message("/start " + kCode, 42));
    CHECK(h.auth.calls.empty());

    h.bot->handle(private_message("/help", kUser));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kHelp));
}

namespace {

// get_updates отдаёт заранее заданные пачки, а когда они кончились —
// поднимает stop, чтобы run_poll_loop вышел. send_message в чат kPoisonChat
// бросает — имитация апдейта, на котором обработка падает.
constexpr std::int64_t kPoisonChat = 666;

class ScriptedTelegram final : public telegram::TelegramApi {
public:
    explicit ScriptedTelegram(std::atomic<bool>& stop) : stop_(stop) {}

    std::vector<telegram::Update> get_updates(std::int64_t offset, int) override {
        offsets_seen.push_back(offset);
        if (batches.empty()) {
            stop_.store(true);
            return {};
        }
        auto batch = batches.front();
        batches.erase(batches.begin());
        return batch;
    }
    std::int64_t send_message(std::int64_t chat_id, const std::string& text, const telegram::Keyboard&) override {
        if (chat_id == kPoisonChat) throw std::runtime_error("send failed");
        sent.emplace_back(chat_id, text);
        return static_cast<std::int64_t>(sent.size());
    }
    void edit_message(std::int64_t, std::int64_t, const std::string&, const telegram::Keyboard&) override {}
    void answer_callback(const std::string&, const std::string&, bool) override {}

    std::vector<std::vector<telegram::Update>> batches;
    std::vector<std::int64_t> offsets_seen;
    std::vector<std::pair<std::int64_t, std::string>> sent;

private:
    std::atomic<bool>& stop_;
};

} // namespace

TEST(poll_loop_advances_offset_and_skips_failing_update) {
    std::atomic<bool> stop{false};
    ScriptedTelegram tg(stop);
    FakeAuth auth;
    FakeReports reports;
    FakeWallet wallet;
    FakeStore store;
    store.offset = 10;
    bot::Bot b(tg, auth, reports, wallet, store, bot::Config{});

    auto help = private_message("/help");
    help.update_id = 10;
    auto poison = private_message("/help", kPoisonChat);
    poison.update_id = 11;
    auto help2 = private_message("/help");
    help2.update_id = 12;
    tg.batches = {{help, poison}, {help2}};

    bot::run_poll_loop(tg, store, b, stop, 1);

    // Первый вызов — с offset из базы, дальше — последний update_id + 1.
    REQUIRE(tg.offsets_seen.size() == 3u);
    CHECK_EQ(tg.offsets_seen[0], static_cast<std::int64_t>(10));
    CHECK_EQ(tg.offsets_seen[1], static_cast<std::int64_t>(12));
    CHECK_EQ(tg.offsets_seen[2], static_cast<std::int64_t>(13));
    CHECK_EQ(store.offset, static_cast<std::int64_t>(13));
    // «Ядовитый» апдейт пропущен, соседние обработаны.
    CHECK_EQ(tg.sent.size(), 2u);
}

TEST(free_text_depends_on_link_state) {
    Harness h;
    h.bot->handle(private_message("350 продукты"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kNotLinked));
    h.bot->handle(private_message("привет"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kNotLinked));

    h.auth.next = ok_exchange();
    h.bot->handle(private_message("/start " + kCode));
    // Не трата — справочник счетов не тянется.
    h.bot->handle(private_message("привет"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnknown));
    CHECK(h.reports.calls.empty());
}

// --- Трата: разбор, resolver, предпросмотр (шаг 6, подшаг 2) ---

namespace {

telegram::Update expense_message(const std::string& text, std::int64_t update_id) {
    auto u = private_message(text);
    u.update_id = update_id;
    return u;
}

// Нажатие кнопки в сообщении message_id (FakeTelegram нумерует с 100).
telegram::Update press(const std::string& data, std::int64_t message_id = 100, std::int64_t from = kUser) {
    telegram::Update u;
    u.update_id = 9000;
    u.has_callback = true;
    u.callback_id = "cb";
    u.callback_data = data;
    u.chat_id = from;
    u.chat_type = "private";
    u.from_id = from;
    u.message_id = message_id;
    return u;
}

// "Кафе=7:c:0|Другая…=7:cp:0|..." — все кнопки клавиатуры по порядку.
std::string buttons(const telegram::Keyboard& kb) {
    std::string out;
    for (const auto& row : kb) {
        for (const auto& b : row) out += (out.empty() ? "" : "|") + b.text + "=" + b.data;
    }
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) {
    bool found = haystack.find(needle) != std::string::npos;
    if (!found) std::cerr << "    не найдено «" << needle << "» в:\n" << haystack << "\n";
    return found;
}

json with_category_count(int n) {
    json a = wallets_json();
    for (int i = 0; i < n; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "Кат %02d", i);
        a["accounts"].push_back({{"uuid", "k-" + std::to_string(i)}, {"name", name}, {"kind", "category"},
                                 {"system_kind", 0}, {"iso_code", ""}, {"symbol", ""}, {"precision", ""}});
    }
    return a;
}

} // namespace

TEST(expense_card_when_everything_resolved) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("350 продукты // молоко", 5));

    REQUIRE(h.reports.calls.size() == 1u);
    CHECK_EQ(h.reports.calls[0].first, std::string("/api/reports/accounts"));
    CHECK(contains(h.last_reply(), "Понял так: 350,00\xC2\xA0₽ · Продукты · Тинькофф · сегодня — молоко"));
    CHECK(contains(h.last_reply(), card::kConfirm));
    // Кошелёк по умолчанию уже есть — «★» не предлагается.
    CHECK_EQ(buttons(h.tg.keyboards.back()),
             std::string("✓ Записать=5:ok|Категория…=5:ce|Кошелёк…=5:we|✕ Не записывать=5:x"));

    // Кошелёк выбран по валюте — это видно в карточке.
    h.bot->handle(expense_message("5 $ кафе и рестораны вчера", 6));
    CHECK(contains(h.last_reply(), "5,00\xC2\xA0$ · Кафе и рестораны · Revolut (по валюте) · вчера"));

    h.bot->handle(expense_message("1 кафе 10.09", 7));
    CHECK(contains(h.last_reply(), "· 10.09"));
    h.bot->handle(expense_message("1 кафе 15.03.2025", 8));
    CHECK(contains(h.last_reply(), "· 15.03.2025"));
}

TEST(expense_asks_category_then_wallet_then_card) {
    Harness h;
    h.link(); // кошелька по умолчанию нет
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("350 каф", 7));

    CHECK(contains(h.last_reply(), "350 · категория? · кошелёк? · сегодня"));
    CHECK(contains(h.last_reply(), "Категория «каф» — какая именно?"));
    CHECK_EQ(buttons(h.tg.keyboards.back()),
             std::string("Кафе=7:c:0|Кафе и рестораны=7:c:1|Другая…=7:cp:0|✕ Не записывать=7:x"));

    h.bot->handle(press("7:c:1"));
    REQUIRE(h.tg.edits.size() == 1u);
    CHECK_EQ(h.tg.edits[0].message_id, static_cast<std::int64_t>(100));
    CHECK(contains(h.tg.edits[0].text, "· Кафе и рестораны · кошелёк?"));
    CHECK(contains(h.tg.edits[0].text, "Какой кошелёк?"));
    CHECK_EQ(buttons(h.tg.edits[0].keyboard), std::string("Тинькофф · ₽=7:w:0|Revolut · $=7:w:1|✕ Не записывать=7:x"));
    REQUIRE(h.tg.answers.size() == 1u);
    CHECK(h.tg.answers[0].text.empty());

    h.bot->handle(press("7:w:1"));
    CHECK(contains(h.tg.edits.back().text, "350,00\xC2\xA0$ · Кафе и рестораны · Revolut · сегодня"));
    CHECK_EQ(buttons(h.tg.edits.back().keyboard),
             std::string("✓ Записать=7:ok|Категория…=7:ce|Кошелёк…=7:we|★ Сделать кошелёк основным=7:wd|✕ Не записывать=7:x"));

    h.bot->handle(press("7:wd"));
    CHECK_EQ(h.store.links.at(kUser).default_wallet_uuid, std::string("w-2"));
    CHECK_EQ(h.tg.answers.back().text, std::string("Кошелёк по умолчанию: Revolut."));
    CHECK(h.tg.edits.back().keyboard.size() == 3u); // без «★»

    h.bot->handle(press("7:x"));
    CHECK_EQ(h.tg.edits.back().text,
             std::string("✕ Не записано: 350,00\xC2\xA0$ · Кафе и рестораны · Revolut · сегодня"));
    CHECK(h.tg.edits.back().keyboard.empty());

    // Черновика больше нет.
    h.bot->handle(press("7:ce"));
    CHECK_EQ(h.tg.answers.back().text, std::string(bot::texts::kDraftStale));
    CHECK_EQ(h.tg.edits.back().text, std::string(bot::texts::kDraftStale));
    // На каждое нажатие — ровно один ответ.
    CHECK_EQ(h.tg.answers.size(), 5u);
}

TEST(expense_category_pages) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", with_category_count(20)}};
    h.bot->handle(expense_message("350", 3));
    CHECK(contains(h.last_reply(), "Какая категория?"));
    // Первые 6 кандидатов + «Другая…» + ✕.
    CHECK(contains(buttons(h.tg.keyboards.back()), "Кат 05=3:c:5|Другая…=3:cp:0|✕"));

    h.bot->handle(press("3:cp:0"));
    CHECK(contains(h.tg.edits.back().text, "Стр. 1 из 3."));
    CHECK(contains(buttons(h.tg.edits.back().keyboard), "Кат 07=3:c:7|Ещё ›=3:cp:1|✕"));
    CHECK(buttons(h.tg.edits.back().keyboard).find("Назад") == std::string::npos);

    h.bot->handle(press("3:cp:99")); // за последней — последняя
    CHECK(contains(h.tg.edits.back().text, "Стр. 3 из 3."));
    std::string last = buttons(h.tg.edits.back().keyboard);
    CHECK(contains(last, "Кат 16=3:c:16|Кат 17=3:c:17|Кат 18=3:c:18|Кат 19=3:c:19|‹ Назад=3:cp:1|✕"));
    CHECK(last.find("Ещё") == std::string::npos);

    h.bot->handle(press("3:c:17"));
    CHECK(contains(h.tg.edits.back().text, "· Кат 17 · Тинькофф ·"));
    CHECK(contains(h.tg.edits.back().text, card::kConfirm));
}

TEST(expense_unknown_category_shows_all) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("350 шаурма у метро", 4));
    CHECK(contains(h.last_reply(), "Нет категории «шаурма». Выберите:"));
    CHECK_EQ(buttons(h.tg.keyboards.back()),
             std::string("Кафе=4:c:0|Кафе и рестораны=4:c:1|Продукты=4:c:2|✕ Не записывать=4:x"));
    CHECK(contains(h.last_reply(), "— у метро"));
}

TEST(expense_currency_mismatch_offers_force) {
    Harness h;
    h.link();
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("12 € кафе @revolut", 9));
    CHECK(contains(h.last_reply(), "Сумма в EUR, а кошелёк «Revolut» — в USD. Кошельков в EUR нет, конвертации тоже."));
    CHECK_EQ(buttons(h.tg.keyboards.back()), std::string("Всё равно в «Revolut»=9:w:1|✕ Не записывать=9:x"));
    h.bot->handle(press("9:w:1"));
    CHECK(contains(h.tg.edits.back().text, "12,00\xC2\xA0$ · Кафе · Revolut · сегодня"));
}

TEST(expense_wallet_change_from_card) {
    Harness h;
    h.link("w-1");
    json acc = accounts_json();
    acc["accounts"].push_back({{"uuid", "w-3"}, {"name", "Иены"}, {"kind", "wallet"}, {"system_kind", 0},
                               {"iso_code", "JPY"}, {"symbol", "¥"}, {"precision", "0"}});
    h.reports.routes = {{"/api/reports/accounts", acc}};

    // Валюта в сообщении: кошелёк другой валюты с карточки — отказ окном.
    h.bot->handle(expense_message("12 $ кафе", 10));
    CHECK(contains(h.last_reply(), "Revolut (по валюте)"));
    h.bot->handle(press("10:we"));
    CHECK_EQ(buttons(h.tg.edits.back().keyboard),
             std::string("Тинькофф · ₽=10:w:0|Revolut · $=10:w:1|Иены · ¥=10:w:2|✕ Не записывать=10:x"));
    std::size_t edits = h.tg.edits.size();
    h.bot->handle(press("10:w:0"));
    CHECK_EQ(h.tg.edits.size(), edits); // карточка не изменилась
    CHECK(h.tg.answers.back().alert);
    CHECK(contains(h.tg.answers.back().text, "Сумма в USD, а кошелёк «Тинькофф» — в RUB. Конвертации нет."));

    // Без валюты в сообщении — валюта просто становится валютой кошелька.
    h.bot->handle(expense_message("10,5 кафе", 11));
    h.bot->handle(press("11:we", 101));
    h.bot->handle(press("11:w:1", 101));
    CHECK_EQ(h.tg.edits.back().message_id, static_cast<std::int64_t>(101));
    CHECK(contains(h.tg.edits.back().text, "10,50\xC2\xA0$ · Кафе · Revolut ·"));

    // В иенах дробной суммы нет.
    h.bot->handle(press("11:we", 101));
    h.bot->handle(press("11:w:2", 101));
    CHECK(h.tg.answers.back().alert);
    CHECK_EQ(h.tg.answers.back().text, std::string("В кошельке «Иены» (JPY) — только целые суммы."));

    // «Категория…» — все категории.
    h.bot->handle(press("11:ce", 101));
    CHECK(contains(buttons(h.tg.edits.back().keyboard), "Продукты=11:c:2"));
    h.bot->handle(press("11:c:2", 101));
    CHECK(contains(h.tg.edits.back().text, "· Продукты · Revolut ·"));
}

TEST(callback_rejects_foreign_bad_and_stale) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("350 кафе", 12));
    std::size_t edits = h.tg.edits.size();

    h.bot->handle(press("12:x", 100, 555)); // чужой пользователь
    h.bot->handle(press("мусор"));
    h.bot->handle(press("12:c:99")); // индекс за границей
    h.bot->handle(press("12:c:1:2"));
    auto group = press("12:x");
    group.chat_type = "group";
    h.bot->handle(group);
    CHECK_EQ(h.tg.edits.size(), edits);
    CHECK_EQ(h.tg.answers.size(), 5u);
    for (const auto& a : h.tg.answers) CHECK(!a.alert);

    // Черновик жив — ✕ срабатывает.
    h.bot->handle(press("12:x"));
    CHECK(contains(h.tg.edits.back().text, "✕ Не записано"));

    // Закрытое тестирование — окно.
    h.config.allowed_user_ids = {kUser};
    h.rebuild();
    h.bot->handle(press("12:x", 100, 42));
    CHECK(h.tg.answers.back().alert);
    CHECK_EQ(h.tg.answers.back().text, std::string(bot::texts::kClosedBeta));
}

TEST(callback_stale_after_ttl_and_relink) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("350 кафе", 13));
    h.now += drafts::kTtlSecs;
    h.bot->handle(press("13:ce"));
    CHECK_EQ(h.tg.answers.back().text, std::string(bot::texts::kDraftStale));

    // Чат переподключили к другому пользователю кошелька — справочник в
    // черновике чужой.
    h.bot->handle(expense_message("350 кафе", 14));
    h.auth.next = ok_exchange("user-2");
    h.bot->handle(private_message("/start " + kCode));
    h.bot->handle(press("14:ce"));
    CHECK_EQ(h.tg.answers.back().text, std::string(bot::texts::kDraftStale));
}

TEST(callback_double_click_and_redelivery) {
    Harness h;
    h.link();
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("350 каф", 15));
    h.bot->handle(press("15:c:0"));
    h.bot->handle(press("15:c:0")); // тот же клик ещё раз — та же правка
    CHECK_EQ(h.tg.answers.size(), 2u);
    CHECK_EQ(h.tg.edits[0].text, h.tg.edits[1].text);

    // Повторная доставка того же апдейта — тот же id черновика в кнопках.
    h.bot->handle(expense_message("350 каф", 15));
    CHECK(contains(buttons(h.tg.keyboards.back()), "=15:c:0"));
}

TEST(expense_rejects_before_card) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};

    h.bot->handle(private_message("12 евро кафе"));
    CHECK_EQ(h.last_reply(), std::string("Нет кошелька в EUR — конвертации нет. Кошельки — /wallets"));

    h.bot->handle(private_message("10,555 кафе"));
    CHECK_EQ(h.last_reply(), std::string("В кошельке «Тинькофф» (RUB) — не больше 2 знаков после запятой."));

    h.bot->handle(private_message("1 кафе 31.02"));
    CHECK(h.last_reply().find("Нет такой даты") != std::string::npos);
    h.bot->handle(private_message("0 кафе"));
    CHECK(h.last_reply().find("больше нуля") != std::string::npos);
    CHECK(contains(metrics::render(), "tg_parse_fail_total{reason=\"zero_amount\"}"));
    CHECK(contains(metrics::render(), "tg_expense_total{result=\"rejected\"}"));

    h.reports.routes = {{"/api/reports/accounts", json::parse(R"({"accounts": []})")}};
    h.bot->handle(private_message("/categories")); // свежий справочник — уже пустой
    h.bot->handle(private_message("350 кафе"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kNoWallets));
}

TEST(accounts_cache_ttl_and_invalidation) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    const std::string kAccounts = "/api/reports/accounts";

    h.bot->handle(private_message("350 кафе"));
    h.bot->handle(private_message("100 продукты"));
    h.bot->handle(private_message("/default"));
    CHECK_EQ(count_calls(h.reports, kAccounts), 1u); // трата, трата, /default — из кеша

    h.now += bot::kAccountsTtlSecs + 1;
    h.bot->handle(private_message("350 кафе"));
    CHECK_EQ(count_calls(h.reports, kAccounts), 2u);

    // /categories — всегда свежий справочник, и он же обновляет кеш.
    h.bot->handle(private_message("/categories"));
    CHECK_EQ(count_calls(h.reports, kAccounts), 3u);
    h.bot->handle(private_message("350 кафе"));
    CHECK_EQ(count_calls(h.reports, kAccounts), 3u);

    // /unlink выбрасывает запись: после переподключения — новый запрос.
    h.bot->handle(private_message("/unlink"));
    h.link("w-1");
    h.bot->handle(private_message("350 кафе"));
    CHECK_EQ(count_calls(h.reports, kAccounts), 4u);

    // /start (в том числе другим пользователем кошелька) — тоже.
    h.auth.next = ok_exchange("user-2");
    h.bot->handle(private_message("/start " + kCode));
    h.bot->handle(private_message("350 кафе"));
    CHECK_EQ(count_calls(h.reports, kAccounts), 5u);
}

// --- Команды чтения (шаг 5) ---

TEST(read_commands_require_link) {
    Harness h;
    for (const char* cmd : {"/wallets", "/categories", "/last", "/default", "/tz", "/unlink"}) {
        h.bot->handle(private_message(cmd));
        CHECK_EQ(h.last_reply(), std::string(bot::texts::kNotLinked));
    }
    CHECK(h.reports.calls.empty());
    CHECK(h.auth.token_calls.empty());
}

TEST(wallets_takes_token_and_lists_balances) {
    Harness h;
    h.link("w-2");
    h.reports.routes = {
        {"/api/reports/accounts", wallets_json()},
        {"/api/reports/balance", json::parse(R"({"accounts": [
            {"account_uuid": "w-1", "balance": "12345.6"},
            {"account_uuid": "w-2", "balance": "-5"}
        ], "total_eq": "0", "incomplete": false, "eq_currency_uuid": "c-usd"})")},
    };
    h.bot->handle(private_message("/wallets"));

    // Кеша не было — один токен у google-auth, по tg_user_id.
    REQUIRE(h.auth.token_calls.size() == 1u);
    CHECK_EQ(h.auth.token_calls[0], kUser);
    // Оба запроса — с полученным токеном; баланс «на завтра» в поясе
    // пользователя (21.09 → date=2026-09-22), eq — валюта кошелька по умолчанию.
    REQUIRE(h.reports.calls.size() == 2u);
    CHECK_EQ(h.reports.calls[1].second, std::string("access-1"));
    CHECK_EQ(h.reports.calls[1].first, std::string("/api/reports/balance?date=2026-09-22&eq_currency=USD"));

    std::string out = h.last_reply();
    CHECK(out.find("• Тинькофф — 12\xE2\x80\xAF" "345,60\xC2\xA0₽") != std::string::npos);
    CHECK(out.find("★ Revolut — \xE2\x88\x92" "5,00\xC2\xA0$") != std::string::npos);
    CHECK(out.find("/default") == std::string::npos); // кошелёк по умолчанию уже выбран

    // Второй вызов в пределах жизни токена — из кеша.
    h.bot->handle(private_message("/wallets"));
    CHECK_EQ(h.auth.token_calls.size(), 1u);
}

// Связку удалили в google-auth (веб «Отключить») — 404 на выпуск токена.
TEST(unlinked_in_google_auth_removes_settings_and_explains) {
    Harness h;
    h.link();
    h.auth.token_status = auth::TokenResult::Status::NotLinked;
    h.bot->handle(private_message("/wallets"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kRevoked));
    CHECK(!h.store.find_link(kUser).has_value());
    CHECK(h.reports.calls.empty());
}

TEST(auth_unavailable_keeps_settings) {
    Harness h;
    h.link();
    h.auth.token_status = auth::TokenResult::Status::Unavailable;
    h.bot->handle(private_message("/categories"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnavailable));
    CHECK(h.store.find_link(kUser).has_value());
}

TEST(report_401_takes_new_token_once_and_retries) {
    Harness h;
    h.link();
    h.reports.routes = {{"/api/reports/accounts", json::parse(R"({"accounts": []})")}};
    h.reports.unauthorized_tokens = {"access-1"}; // первый выданный токен «не принят»
    h.bot->handle(private_message("/categories"));
    CHECK_EQ(h.auth.token_calls.size(), 2u);
    CHECK(h.store.find_link(kUser).has_value());
    REQUIRE(h.reports.calls.size() == 2u);
    CHECK_EQ(h.reports.calls[1].second, std::string("access-2"));
    CHECK(h.last_reply().find("Категорий пока нет") != std::string::npos);
}

TEST(report_401_twice_gives_up_without_loop) {
    Harness h;
    h.link();
    h.reports.routes = {{"/api/reports/accounts", json::parse(R"({"accounts": []})")}};
    h.reports.unauthorized_tokens = {"access-1", "access-2", "access-3"};
    h.bot->handle(private_message("/categories"));
    CHECK_EQ(h.auth.token_calls.size(), 2u); // ровно один повтор
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnavailable));
}

TEST(categories_skip_system_ones) {
    Harness h;
    h.link();
    h.reports.routes = {{"/api/reports/accounts", json::parse(R"({"accounts": [
        {"uuid": "c-1", "name": "Продукты", "kind": "category", "system_kind": 0},
        {"uuid": "c-2", "name": "Transfer", "kind": "category", "system_kind": 1},
        {"uuid": "c-3", "name": "Кафе", "kind": "category", "system_kind": 0}
    ]})")}};
    h.bot->handle(private_message("/categories"));
    std::string out = h.last_reply();
    CHECK(out.find("Категории (2)") != std::string::npos);
    CHECK(out.find("• Продукты") != std::string::npos);
    CHECK(out.find("Transfer") == std::string::npos);
}

TEST(last_formats_expense_income_transfer_and_limits) {
    Harness h;
    h.link();
    h.reports.routes = {
        {"/api/reports/accounts", wallets_json()},
        {"/api/reports/operations", json::parse(R"({"operations": [
            {"date": "2026-09-21T09:00:00", "kind": "expense", "amount": "350.00", "symbol": "₽", "precision": "2",
             "category_name": "Продукты", "wallet_name": "Тинькофф", "comment": "обед"},
            {"date": "2026-09-20T12:00:00", "kind": "income", "amount": "1000", "symbol": "₽", "precision": "2",
             "category_name": "Зарплата", "wallet_name": "Тинькофф", "comment": ""},
            {"date": "2026-09-19T12:00:00", "kind": "transfer", "amount": "50", "symbol": "$", "precision": "2",
             "from_name": "Revolut", "to_name": "Тинькофф", "comment": ""}
        ], "has_more": true})")},
    };
    h.bot->handle(private_message("/last 3"));
    REQUIRE(h.reports.calls.size() == 2u);
    CHECK(h.reports.calls[1].first.find("limit=3") != std::string::npos);
    CHECK(h.reports.calls[1].first.find("to=2026-09-22T00:00:00") != std::string::npos);
    std::string out = h.last_reply();
    CHECK(out.find("21.09  \xE2\x88\x92" "350,00\xC2\xA0₽  Продукты · Тинькофф — обед") != std::string::npos);
    CHECK(out.find("20.09  1\xE2\x80\xAF" "000,00\xC2\xA0₽  Зарплата → Тинькофф") != std::string::npos);
    CHECK(out.find("19.09  50,00\xC2\xA0$  Revolut → Тинькофф") != std::string::npos);

    h.bot->handle(private_message("/last abc"));
    CHECK(h.last_reply().find("Использование") != std::string::npos);
}

TEST(default_sets_by_prefix_and_reports_ambiguity) {
    Harness h;
    h.link();
    json three = wallets_json();
    three["accounts"].push_back({{"uuid", "w-3"}, {"name", "Тинькофф Black"}, {"kind", "wallet"}, {"iso_code", "RUB"},
                                 {"symbol", "₽"}, {"precision", "2"}});
    h.reports.routes = {{"/api/reports/accounts", three}};

    h.bot->handle(private_message("/default REV"));
    CHECK_EQ(h.store.links.at(kUser).default_wallet_uuid, std::string("w-2"));
    CHECK(h.last_reply().find("Revolut") != std::string::npos);

    // "тинькофф" — точное совпадение побеждает префикс "Тинькофф Black".
    h.bot->handle(private_message("/default ТИНЬКОФФ"));
    CHECK_EQ(h.store.links.at(kUser).default_wallet_uuid, std::string("w-1"));

    h.bot->handle(private_message("/default тин"));
    CHECK(h.last_reply().find("Подходит несколько") != std::string::npos);
    CHECK_EQ(h.store.links.at(kUser).default_wallet_uuid, std::string("w-1")); // не изменился

    h.bot->handle(private_message("/default сбер"));
    CHECK(h.last_reply().find("Нет кошелька «сбер»") != std::string::npos);
}

TEST(tz_validates_and_saves) {
    Harness h;
    h.link();
    h.bot->handle(private_message("/tz Asia/Tbilisi"));
    CHECK_EQ(h.store.links.at(kUser).tz, std::string("Asia/Tbilisi"));
    // Часы Harness: 2026-09-21 14:13 UTC → Тбилиси UTC+4.
    CHECK_EQ(h.last_reply(), std::string("Часовой пояс: Asia/Tbilisi. Сейчас там 21.09, 18:13."));

    h.bot->handle(private_message("/tz Mars/Olympus"));
    CHECK(h.last_reply().find("Не знаю пояса") != std::string::npos);
    CHECK_EQ(h.store.links.at(kUser).tz, std::string("Asia/Tbilisi"));

    h.bot->handle(private_message("/tz ../../etc/passwd"));
    CHECK_EQ(h.store.links.at(kUser).tz, std::string("Asia/Tbilisi"));

    h.bot->handle(private_message("/tz"));
    CHECK(h.last_reply().find("Часовой пояс: Asia/Tbilisi") != std::string::npos);
}

TEST(unlink_deletes_link_in_google_auth_and_settings) {
    Harness h;
    h.link();
    h.bot->handle(private_message("/unlink"));
    REQUIRE(h.auth.unlinks.size() == 1u);
    CHECK_EQ(h.auth.unlinks[0], kUser);
    CHECK(!h.store.find_link(kUser).has_value());
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnlinked));
}

// google-auth недоступен — не делаем вид, что отключили: связка там жива,
// бот продолжил бы получать токены.
TEST(unlink_when_auth_unavailable_keeps_settings) {
    Harness h;
    h.link();
    h.auth.unlink_ok = false;
    h.bot->handle(private_message("/unlink"));
    CHECK(h.store.find_link(kUser).has_value());
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnavailable));
}

// --- Запись (шаг 6, подшаг 5) ---

namespace {

wallet::PushResult push_result(wallet::PushResult::Status status) {
    wallet::PushResult r;
    r.status = status;
    r.http_status = status == wallet::PushResult::Status::Ok ? 200 : 0;
    r.seq = 7;
    return r;
}

json without_category(const std::string& uuid) {
    json a = accounts_json();
    json out = json::array();
    for (const auto& row : a["accounts"]) {
        if (row["uuid"] != uuid) out.push_back(row);
    }
    a["accounts"] = out;
    return a;
}

} // namespace

TEST(save_writes_expense_and_keeps_done_draft) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.wallet.next = push_result(wallet::PushResult::Status::Ok);
    h.bot->handle(expense_message("350,5 продукты // молоко", 20));
    h.bot->handle(press("20:ok"));

    REQUIRE(h.wallet.calls.size() == 1u);
    const wallet::Expense& e = h.wallet.calls[0].first;
    CHECK_EQ(h.wallet.calls[0].second, std::string("access-1"));
    CHECK_EQ(e.wallet_uuid, std::string("w-1"));
    CHECK_EQ(e.category_uuid, std::string("c-3"));
    CHECK_EQ(e.amount, std::string("350.5"));
    CHECK_EQ(e.comment, std::string("молоко"));
    CHECK(contains(e.date, "2026-09-21 17:13")); // «сейчас» в поясе пользователя (Москва)
    CHECK(uuidgen::looks_valid(e.batch_id));
    CHECK(uuidgen::looks_valid(e.op_uuid));
    CHECK(e.batch_id != e.op_uuid);
    // Перед записью — свежий справочник: создание черновика + сверка.
    CHECK_EQ(count_calls(h.reports, "/api/reports/accounts"), 2u);

    CHECK_EQ(h.tg.edits.back().text, std::string("✓ Записано: 350,50\xC2\xA0₽ · Продукты · Тинькофф · сегодня — молоко"));
    CHECK(h.tg.edits.back().keyboard.empty());
    CHECK_EQ(h.tg.answers.back().text, std::string("Записано."));

    // Повторный клик — «уже записано», без второго push и без правки.
    std::size_t edits = h.tg.edits.size();
    h.bot->handle(press("20:ok"));
    h.bot->handle(press("20:ce"));
    CHECK_EQ(h.wallet.calls.size(), 1u);
    CHECK_EQ(h.tg.edits.size(), edits);
    CHECK_EQ(h.tg.answers.back().text, std::string("Уже записано."));
    // Метрики §3.4 (глобальные на процесс — проверяем, что ряды есть).
    std::string m = metrics::render();
    CHECK(contains(m, "tg_expense_total{result=\"resolved\"}"));
    CHECK(contains(m, "tg_expense_save_total{result=\"saved\"}"));
    CHECK(contains(m, "tg_expense_save_total{result=\"done\"}"));
    CHECK(contains(m, "tg_callbacks_total{result=\"saved\"}"));
}

TEST(save_batch_ids_are_stable_per_draft_and_distinct_otherwise) {
    auto batch_of = [](const std::string& bot_username, std::int64_t update_id) {
        Harness h;
        h.config.bot_username = bot_username;
        h.rebuild();
        h.link("w-1");
        h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
        h.wallet.next = push_result(wallet::PushResult::Status::Unavailable);
        h.bot->handle(expense_message("350 кафе", update_id));
        h.bot->handle(press(std::to_string(update_id) + ":ok"));
        h.bot->handle(press(std::to_string(update_id) + ":ok")); // повтор после неясного исхода
        REQUIRE(h.wallet.calls.size() == 2u);
        CHECK_EQ(h.wallet.calls[0].first.batch_id, h.wallet.calls[1].first.batch_id);
        CHECK_EQ(h.wallet.calls[0].first.op_uuid, h.wallet.calls[1].first.op_uuid);
        return h.wallet.calls[0].first.batch_id;
    };
    std::string a = batch_of("WalletTransaction_bot", 30);
    CHECK_EQ(batch_of("WalletTransaction_bot", 30), a); // детерминированно, между запусками тоже
    CHECK(batch_of("WalletTransaction_bot", 31) != a);
    CHECK(batch_of("WalletTransaction_dev_bot", 30) != a); // dev- и prod-бот с одним update_id
}

TEST(save_unavailable_locks_draft_until_checked) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.wallet.next = push_result(wallet::PushResult::Status::Unavailable);
    h.bot->handle(expense_message("350 кафе", 21));
    h.bot->handle(press("21:ok"));

    CHECK(contains(h.tg.edits.back().text, "возможно, трата уже записана"));
    CHECK_EQ(buttons(h.tg.edits.back().keyboard), std::string("✓ Проверить ещё раз=21:ok"));
    CHECK(h.tg.answers.back().alert);

    // Править нельзя — исход первой попытки неизвестен.
    std::size_t edits = h.tg.edits.size();
    h.bot->handle(press("21:ce"));
    h.bot->handle(press("21:x"));
    CHECK_EQ(h.tg.edits.size(), edits);
    CHECK_EQ(h.wallet.calls.size(), 1u);
    CHECK(h.tg.answers.back().alert);

    std::size_t accounts_calls = count_calls(h.reports, "/api/reports/accounts");
    h.wallet.next = push_result(wallet::PushResult::Status::Ok);
    h.bot->handle(press("21:ok"));
    CHECK_EQ(h.wallet.calls.size(), 2u);
    CHECK_EQ(count_calls(h.reports, "/api/reports/accounts"), accounts_calls); // без повторной сверки
    CHECK(contains(h.tg.edits.back().text, "✓ Записано: 350,00"));
}

TEST(save_already_exists_counts_as_saved) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.wallet.next = push_result(wallet::PushResult::Status::AlreadyExists);
    h.bot->handle(expense_message("350 кафе", 23));
    h.bot->handle(press("23:ok"));
    CHECK(contains(h.tg.edits.back().text, "✓ Записано:"));
}

TEST(save_rechecks_accounts_and_asks_again_when_category_deleted) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.wallet.next = push_result(wallet::PushResult::Status::Ok);
    h.bot->handle(expense_message("350 продукты", 24));

    // «Продукты» удалили на телефоне.
    h.reports.routes = {{"/api/reports/accounts", without_category("c-3")}};
    h.bot->handle(press("24:ok"));
    CHECK(h.wallet.calls.empty());
    CHECK(contains(h.tg.edits.back().text, "категория?"));
    CHECK_EQ(buttons(h.tg.edits.back().keyboard), std::string("Кафе=24:c:0|Кафе и рестораны=24:c:1|✕ Не записывать=24:x"));
    CHECK(h.tg.answers.back().alert);
    CHECK(contains(h.tg.answers.back().text, "удалили в приложении"));

    h.bot->handle(press("24:c:0"));
    h.bot->handle(press("24:ok"));
    REQUIRE(h.wallet.calls.size() == 1u);
    CHECK_EQ(h.wallet.calls[0].first.category_uuid, std::string("c-1"));
}

TEST(save_missing_reference_asks_again_and_drops_cache) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.wallet.next = push_result(wallet::PushResult::Status::MissingReference);
    h.bot->handle(expense_message("350 кафе", 25));
    h.bot->handle(press("25:ok"));
    CHECK_EQ(h.wallet.calls.size(), 1u);
    // создание + сверка + свежий справочник после 409
    CHECK_EQ(count_calls(h.reports, "/api/reports/accounts"), 3u);
    CHECK(contains(h.tg.edits.back().text, "Какая категория?"));
    CHECK(h.tg.answers.back().alert);
}

TEST(save_reader_and_rejected_give_up) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};

    h.wallet.next = push_result(wallet::PushResult::Status::Reader);
    h.bot->handle(expense_message("350 кафе", 26));
    h.bot->handle(press("26:ok"));
    CHECK(contains(h.tg.edits.back().text, "✕ Не записано: 350,00"));
    CHECK(contains(h.tg.edits.back().text, "роль «читатель»"));
    CHECK(h.tg.edits.back().keyboard.empty());
    h.bot->handle(press("26:ok"));
    CHECK_EQ(h.tg.answers.back().text, std::string(bot::texts::kDraftStale));

    h.wallet.next = push_result(wallet::PushResult::Status::Rejected);
    h.bot->handle(expense_message("350 кафе", 27));
    h.bot->handle(press("27:ok", 101));
    CHECK(contains(h.tg.edits.back().text, "ошибка сервиса, внесите трату в приложении"));
    CHECK_EQ(h.wallet.calls.size(), 2u);
}

TEST(save_retries_once_on_wallet_api_401) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.wallet.next = push_result(wallet::PushResult::Status::Ok);
    h.wallet.unauthorized_tokens = {"access-1"};
    h.bot->handle(expense_message("350 кафе", 28));
    h.bot->handle(press("28:ok"));
    REQUIRE(h.wallet.calls.size() == 2u);
    CHECK_EQ(h.wallet.calls[1].second, std::string("access-2"));
    CHECK_EQ(h.wallet.calls[0].first.batch_id, h.wallet.calls[1].first.batch_id);
    CHECK(contains(h.tg.edits.back().text, "✓ Записано:"));
}


// --- Свободный текст через wallet-ai (шаг 4 org/docs/TODO_wallet_ai.md) ---

namespace {

class FakeAi final : public ai::AiApi {
public:
    struct Create {
        std::string token;
        json input;
        std::string key;
    };
    ai::CreateResult create_parse_expense(const std::string& token, const json& input,
                                          const std::string& key) override {
        creates.push_back({token, input, key});
        if (unauthorized_tokens.count(token) != 0) {
            ai::CreateResult r;
            r.status = ai::CreateResult::Status::Unauthorized;
            return r;
        }
        return create_next;
    }
    ai::JobResult get_job(const std::string& token, const std::string& job_id) override {
        gets.emplace_back(token, job_id);
        return job_next;
    }
    void cancel_job(const std::string&, const std::string& job_id) override { cancels.push_back(job_id); }
    std::optional<bool> enabled(const std::string&) override {
        ++enabled_calls;
        return enabled_value;
    }

    std::vector<Create> creates;
    std::vector<std::pair<std::string, std::string>> gets;
    std::vector<std::string> cancels;
    ai::CreateResult create_next = created("job-1");
    ai::JobResult job_next;
    std::unordered_set<std::string> unauthorized_tokens;
    std::optional<bool> enabled_value = true;
    int enabled_calls = 0;

    static ai::CreateResult created(const std::string& id) {
        ai::CreateResult r;
        r.status = ai::CreateResult::Status::Created;
        r.job_id = id;
        return r;
    }
};

ai::JobResult job(ai::JobResult::Status status, json result = json::object(), const std::string& failure = "") {
    ai::JobResult r;
    r.status = status;
    r.result = std::move(result);
    r.failure = failure;
    return r;
}

json draft_result(const std::string& amount, const std::string& category, const std::string& wallet = "") {
    return {{"outcome", "draft"},
            {"expense",
             {{"amount", amount},
              {"fraction_digits", 0},
              {"currency_hint", ""},
              {"category", category},
              {"wallet", wallet},
              {"datetime", "2026-09-21 17:13:20"},
              {"comment", "с круассаном"}}},
            {"prompt_version", "expense-v2"}};
}

struct AiHarness : Harness {
    FakeAi ai;
    AiHarness() {
        link("w-1");
        reports.routes = {{"/api/reports/accounts", accounts_json()}};
        bot->enable_ai(&ai);
    }
};

} // namespace

TEST(ai_free_text_parsing_then_card_then_save) {
    AiHarness h;
    h.bot->handle(expense_message("кофе с круассаном 200", 30));

    REQUIRE(h.ai.creates.size() == 1u);
    const auto& c = h.ai.creates[0];
    CHECK_EQ(c.input["text"].get<std::string>(), std::string("кофе с круассаном 200"));
    CHECK_EQ(c.input["now"]["date"].get<std::string>(), std::string("2026-09-21")); // пояс пользователя — Москва
    CHECK_EQ(c.input["now"]["time"].get<std::string>(), std::string("17:13:20"));
    CHECK_EQ(c.input["default_wallet_uuid"].get<std::string>(), std::string("w-1"));
    CHECK_EQ(c.key, "tg:" + std::to_string(kUser) + ":30"); // повтор апдейта — то же задание
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kAiParsing));
    CHECK(h.bot->has_pending_ai());

    // Ещё думает — ничего не правим.
    h.ai.job_next = job(ai::JobResult::Status::Pending);
    h.bot->ai_tick();
    CHECK(h.tg.edits.empty());
    CHECK(h.bot->has_pending_ai());

    // Готово — «Разбираю…» (message_id 100) становится карточкой с пометкой.
    h.ai.job_next = job(ai::JobResult::Status::Done, draft_result("200", "Кафе"));
    h.bot->ai_tick();
    CHECK(!h.bot->has_pending_ai());
    REQUIRE(!h.tg.edits.empty());
    CHECK_EQ(h.tg.edits.back().message_id, 100);
    CHECK(contains(h.tg.edits.back().text,
                   "Понял из текста — проверьте перед записью: 200,00\xC2\xA0₽ · Кафе · Тинькофф · сегодня"));
    CHECK_EQ(buttons(h.tg.edits.back().keyboard),
             std::string("✓ Записать=30:ok|Категория…=30:ce|Кошелёк…=30:we|✕ Не записывать=30:x"));
    CHECK_EQ(h.tg.sent.size(), 1u); // нового сообщения нет — только «Разбираю…»

    // Запись — тем же путём, что у строгого формата.
    h.wallet.next = push_result(wallet::PushResult::Status::Ok);
    h.bot->handle(press("30:ok"));
    REQUIRE(h.wallet.calls.size() == 1u);
    CHECK_EQ(h.wallet.calls[0].first.category_uuid, std::string("c-1"));
    CHECK_EQ(h.wallet.calls[0].first.wallet_uuid, std::string("w-1"));
    CHECK_EQ(h.wallet.calls[0].first.amount, std::string("200"));
    CHECK(contains(h.tg.edits.back().text, "✓ Записано:"));
}

TEST(ai_not_called_for_strict_ok_no_digit_or_long_text) {
    AiHarness h;
    h.bot->handle(expense_message("350 продукты", 31));
    CHECK(contains(h.last_reply(), "Понял так: 350,00"));
    h.bot->handle(expense_message("привет", 32));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnknown));
    h.bot->handle(expense_message("кофе " + std::string(400, 'x') + " 200", 33));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnknown));
    // Ноль и будущая дата заданы явно и явно неверны — ИИ не поможет.
    h.bot->handle(expense_message("0 кафе", 34));
    h.bot->handle(expense_message("100 кафе 31.12.2099", 35));
    CHECK(h.ai.creates.empty());
}

TEST(ai_disabled_behaves_as_before) {
    Harness h;
    h.link("w-1");
    h.reports.routes = {{"/api/reports/accounts", accounts_json()}};
    h.bot->handle(expense_message("кофе 200", 36));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kUnknown));
    CHECK(!h.bot->has_pending_ai());
}

TEST(ai_unavailable_falls_back_immediately) {
    AiHarness h;
    h.ai.create_next.status = ai::CreateResult::Status::Unavailable;
    h.bot->handle(expense_message("кофе 200", 37));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kAiNotUnderstood));
    CHECK(!h.bot->has_pending_ai());

    // Категория не нашлась + ИИ занят — прежние кнопки строгого разбора сразу.
    h.ai.create_next.status = ai::CreateResult::Status::Busy;
    h.bot->handle(expense_message("350 потратил на кофе", 38));
    CHECK(contains(h.last_reply(), "Понял так: 350,00"));
    CHECK(!h.tg.keyboards.back().empty());
}

TEST(ai_category_not_found_falls_back_to_strict_buttons_by_edit) {
    AiHarness h;
    h.bot->handle(expense_message("350 потратил на кофе", 39));
    REQUIRE(h.ai.creates.size() == 1u);
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kAiParsing));

    h.ai.job_next = job(ai::JobResult::Status::Failed, json::object(), "timeout");
    h.bot->ai_tick();
    REQUIRE(!h.tg.edits.empty());
    CHECK_EQ(h.tg.edits.back().message_id, 100);
    CHECK(contains(h.tg.edits.back().text, "Понял так: 350,00")); // строгий разбор, без пометки ИИ
    CHECK(!h.tg.edits.back().keyboard.empty());                  // вопрос о категории кнопками
}

TEST(ai_rejected_not_expense_expired_bad_amount) {
    AiHarness h;
    h.bot->handle(expense_message("200 и 450 такси вчера кофе", 40)); // строгий: категория «и» не найдена
    h.ai.job_next = job(ai::JobResult::Status::Done, {{"outcome", "rejected"}, {"reasons", {"amount_not_in_text"}}});
    h.bot->ai_tick();
    CHECK(contains(h.tg.edits.back().text, "Понял так: 200,00"));

    h.ai.create_next = FakeAi::created("job-2");
    h.bot->handle(expense_message("кофе 200", 41));
    h.ai.job_next = job(ai::JobResult::Status::Done, {{"outcome", "not_expense"}});
    h.bot->ai_tick();
    CHECK_EQ(h.tg.edits.back().text, std::string(bot::texts::kAiNotUnderstood));

    // Сумма не разобралась строгим форматом — ИИ; задание истекло — прежний текст ошибки.
    h.ai.create_next = FakeAi::created("job-3");
    h.bot->handle(expense_message("1,200.50 кафе", 42));
    REQUIRE(h.ai.creates.size() == 3u);
    h.ai.job_next = job(ai::JobResult::Status::NotFound);
    h.bot->ai_tick();
    CHECK(contains(h.tg.edits.back().text, "Не понял сумму"));
    CHECK(!h.bot->has_pending_ai());
}

TEST(ai_gives_up_after_timeout_and_cancels) {
    AiHarness h;
    h.bot->handle(expense_message("кофе 200", 43));
    h.ai.job_next = job(ai::JobResult::Status::Pending);
    h.now += bot::kAiGiveUpSecs - 1;
    h.bot->ai_tick();
    CHECK(h.bot->has_pending_ai());
    h.now += 1;
    h.bot->ai_tick();
    CHECK(!h.bot->has_pending_ai());
    REQUIRE(h.ai.cancels.size() == 1u);
    CHECK_EQ(h.ai.cancels[0], std::string("job-1"));
    CHECK_EQ(h.tg.edits.back().text, std::string(bot::texts::kAiNotUnderstood));
}

TEST(ai_create_retries_once_on_401) {
    AiHarness h;
    h.ai.unauthorized_tokens = {"access-1"};
    h.bot->handle(expense_message("кофе 200", 44));
    REQUIRE(h.ai.creates.size() == 2u);
    CHECK_EQ(h.ai.creates[1].token, std::string("access-2"));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kAiParsing));
}

TEST(ai_help_line_only_when_enabled) {
    AiHarness h;
    h.bot->handle(private_message("/help"));
    CHECK(contains(h.last_reply(), bot::texts::kHelpAi));
    CHECK(contains(h.last_reply(), "/wallets")); // остальная справка на месте

    h.ai.enabled_value = false;
    h.now += 61; // кеш статуса — минута
    h.bot->handle(private_message("/help"));
    CHECK(!contains(h.last_reply(), bot::texts::kHelpAi));
    CHECK_EQ(h.last_reply(), std::string(bot::texts::kHelp));
}
