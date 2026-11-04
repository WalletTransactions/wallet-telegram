// Интеграционный сценарий бота (org/docs/TODO_telegram_bot.md, §5 и шаг 8):
// настоящие google-auth, wallet-report, wallet-api и их Postgres/Redis из
// dev-стека, настоящие HTTP-клиенты бота — фейковые только Telegram и своя
// база бота (LinkStore в памяти: её схему проверяют юнит-тесты и migrate.sh).
//
// Пользователя в google-auth и код связывания в Redis готовит run_local.sh
// (google-auth пускает на link-code только токены, выданные им самим), а
// «телефонный» токен выпускает mint_loadtest_token — wallet-api и
// wallet-report проверяют токены только по JWKS.
//
// Сценарий: телефон заводит кошелёк и категорию → /start <код> →
// /categories, /wallets → трата → «✓ Записать» (дважды) → телефон видит одну
// операцию через /sync/pull → токен бота не правит её, не пишет счета и не
// читает pull → /unlink → уборка (DELETE /api/account).
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "bot.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "test_framework.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        throw std::runtime_error(std::string(name) + " не задан — запускайте через tests_integration/run_local.sh");
    }
    return v;
}

struct Env {
    std::string auth_url = env("AUTH_SERVICE_URL");
    std::string report_url = env("WALLET_REPORT_URL");
    std::string api_url = env("WALLET_API_URL");
    std::string service_secret = env("TELEGRAM_SERVICE_SECRET");
    std::string link_code = env("IT_LINK_CODE");
    std::string user_id = env("IT_USER_ID");
    std::string phone_token = env("IT_PHONE_TOKEN");
    std::int64_t tg_user_id = std::stoll(env("IT_TG_USER_ID"));
};

class FakeTelegram final : public telegram::TelegramApi {
public:
    std::vector<telegram::Update> get_updates(std::int64_t, int) override { return {}; }
    std::int64_t send_message(std::int64_t, const std::string& text, const telegram::Keyboard& kb) override {
        sent.push_back(text);
        keyboards.push_back(kb);
        return static_cast<std::int64_t>(sent.size());
    }
    void edit_message(std::int64_t, std::int64_t, const std::string& text, const telegram::Keyboard&) override {
        edits.push_back(text);
    }
    void answer_callback(const std::string&, const std::string& text, bool) override { answers.push_back(text); }
    void set_my_commands(const std::vector<telegram::BotCommand>&) override {}

    std::vector<std::string> sent;
    std::vector<telegram::Keyboard> keyboards;
    std::vector<std::string> edits;
    std::vector<std::string> answers;
};

class MemoryStore final : public links::LinkStore {
public:
    void save_link(const links::Link& link) override { links[link.tg_user_id] = link; }
    std::optional<links::Link> find_link(std::int64_t id) override {
        auto it = links.find(id);
        if (it == links.end()) return std::nullopt;
        return it->second;
    }
    void set_default_wallet(std::int64_t id, const std::string& uuid) override { links.at(id).default_wallet_uuid = uuid; }
    void set_tz(std::int64_t id, const std::string& tz) override { links.at(id).tz = tz; }
    void delete_link(std::int64_t id) override { links.erase(id); }
    std::int64_t load_offset() override { return 0; }
    void save_offset(std::int64_t) override {}

    std::map<std::int64_t, links::Link> links;
};

// Запросы «телефона» и сырые запросы с токеном бота — напрямую httplib.
struct Http {
    int status = 0;
    json body;
};

Http request(const std::string& base, const std::string& method, const std::string& path, const std::string& token,
             const std::string& body = "", const std::string& content_type = "application/json") {
    httplib::Client client(base);
    client.set_read_timeout(15, 0);
    httplib::Headers headers = {{"Authorization", "Bearer " + token}};
    httplib::Result res = method == "GET"      ? client.Get(path, headers)
                          : method == "DELETE" ? client.Delete(path, headers)
                                               : client.Post(path, headers, body, content_type);
    Http out;
    if (!res) return out;
    out.status = res->status;
    out.body = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
    return out;
}

telegram::Update message(const Env& e, const std::string& text, std::int64_t update_id) {
    telegram::Update u;
    u.update_id = update_id;
    u.has_message = true;
    u.chat_id = e.tg_user_id;
    u.chat_type = "private";
    u.from_id = e.tg_user_id;
    u.from_username = "integration_test";
    u.text = text;
    return u;
}

telegram::Update press(const Env& e, const std::string& data) {
    telegram::Update u = message(e, "", 900000);
    u.has_message = false;
    u.has_callback = true;
    u.callback_id = "cb";
    u.callback_data = data;
    u.message_id = 1;
    return u;
}

bool contains(const std::string& haystack, const std::string& needle) {
    bool found = haystack.find(needle) != std::string::npos;
    if (!found) std::cerr << "    не найдено «" << needle << "» в:\n" << haystack << "\n";
    return found;
}

std::string button_data(const telegram::Keyboard& kb, const std::string& text) {
    for (const auto& row : kb) {
        for (const auto& b : row) {
            if (b.text == text) return b.data;
        }
    }
    return {};
}

std::vector<json> operations(const Env& e) {
    // reference_seq — максимум: справочный поток тесту не нужен.
    Http pull = request(e.api_url, "GET", "/sync/pull?ledger_seq=0&reference_seq=9223372036854775807&limit=1000",
                        e.phone_token);
    std::vector<json> out;
    if (pull.status != 200) return out;
    for (const auto& op : pull.body["ledger"]["changes"].value("operation", json::array())) out.push_back(op);
    return out;
}

// Леджер пользователя удаляется целиком, что бы ни случилось в тесте выше.
struct DeleteAccountOnExit {
    const Env& e;
    ~DeleteAccountOnExit() {
        Http r = request(e.api_url, "DELETE", "/api/account", e.phone_token);
        if (r.status != 204) std::cerr << "    уборка: DELETE /api/account → " << r.status << "\n";
    }
};

} // namespace

TEST(telegram_bot_writes_expense_end_to_end) {
    Env e;
    DeleteAccountOnExit cleanup{e};

    // --- Телефон: иконка, валюта, кошелёк «Наличные» (₽) и категория «Продукты».
    std::string suffix = uuidgen::v4().substr(0, 8);
    Http blob = request(e.api_url, "POST", "/sync/blobs", e.phone_token, "it-icon-" + suffix, "application/octet-stream");
    REQUIRE(blob.status == 200);
    std::string sha = blob.body.at("sha256").get<std::string>();

    std::string icon = uuidgen::v4(), currency = uuidgen::v4(), wallet = uuidgen::v4(), category = uuidgen::v4();
    json seed = {
        {"batch_id", uuidgen::v4()},
        {"changes",
         {{"icon", json::array({{{"uuid", icon}, {"name", "it-" + suffix}, {"content_sha256", sha}, {"icon_type", 0}}})},
          {"currency", json::array({{{"uuid", currency},
                                     {"name", "Рубль " + suffix},
                                     {"iso_code", "RUB"},
                                     {"icon_uuid", icon},
                                     {"symbol", "₽"},
                                     {"precision", 2}}})},
          {"account", json::array({{{"uuid", wallet}, {"name", "Наличные"}, {"comment", ""}, {"icon_uuid", icon},
                                    {"is_archived", false}, {"system_kind", 0}, {"kind", 1}},
                                   {{"uuid", category}, {"name", "Продукты"}, {"comment", ""}, {"icon_uuid", icon},
                                    {"is_archived", false}, {"system_kind", 0}, {"kind", 0}}})},
          {"account_operating", json::array({{{"uuid", wallet}, {"currency_uuid", currency}}})}}},
    };
    Http seeded = request(e.api_url, "POST", "/sync/push", e.phone_token, seed.dump());
    REQUIRE(seeded.status == 200);

    // --- Бот с настоящими клиентами.
    FakeTelegram tg;
    MemoryStore store;
    auto auth_api = auth::make_http_api(e.auth_url, e.service_secret);
    auto reports_api = reports::make_http_api(e.report_url, "tg_reports_errors_total");
    auto wallet_api = wallet::make_http_api(e.api_url);
    bot::Config config;
    config.bot_username = "WalletTransaction_it_bot";
    config.default_tz = "Europe/Moscow";
    bot::Bot b(tg, *auth_api, *reports_api, *wallet_api, store, config);

    b.handle(message(e, "/start " + e.link_code, 1));
    REQUIRE(!tg.sent.empty());
    CHECK_EQ(tg.sent.back(), std::string(bot::texts::kLinked));
    REQUIRE(store.find_link(e.tg_user_id).has_value());
    CHECK_EQ(store.links.at(e.tg_user_id).user_id, e.user_id);
    std::string bot_device = store.links.at(e.tg_user_id).device_id;

    b.handle(message(e, "/categories", 2));
    CHECK(contains(tg.sent.back(), "• Продукты"));
    b.handle(message(e, "/wallets", 3));
    CHECK(contains(tg.sent.back(), "Наличные — 0,00"));

    // --- Трата: кошелёк один — сразу карточка.
    b.handle(message(e, "350,5 продукты // интеграционный тест", 4));
    CHECK(contains(tg.sent.back(), "350,50\xC2\xA0₽ · Продукты · Наличные · сегодня"));
    std::string save = button_data(tg.keyboards.back(), "✓ Записать");
    REQUIRE(save == "4:ok");

    b.handle(press(e, save));
    REQUIRE(!tg.edits.empty());
    CHECK(contains(tg.edits.back(), "✓ Записано: 350,50\xC2\xA0₽ · Продукты · Наличные"));
    CHECK_EQ(tg.answers.back(), std::string("Записано."));
    b.handle(press(e, save)); // двойной клик
    CHECK_EQ(tg.answers.back(), std::string("Уже записано."));

    // --- Телефон видит ровно одну операцию, от устройства бота.
    auto ops = operations(e);
    REQUIRE(ops.size() == 1u);
    const json& op = ops[0];
    CHECK_EQ(op.value("acc_fr_uuid", std::string{}), wallet);
    CHECK_EQ(op.value("acc_to_uuid", std::string{}), category);
    CHECK(contains(op.value("amount", std::string{}), "350.5"));
    CHECK_EQ(op.value("comment", std::string{}), std::string("интеграционный тест"));
    if (op.contains("updated_by")) CHECK_EQ(op.value("updated_by", std::string{}), bot_device);
    std::string op_uuid = op.value("uuid", std::string{});

    // --- Границы токена бота (expense:write).
    auth::TokenResult bot_token = auth_api->token(e.tg_user_id);
    REQUIRE(bot_token.status == auth::TokenResult::Status::Ok);

    wallet::Expense edit;
    edit.batch_id = uuidgen::v4();
    edit.op_uuid = op_uuid;
    edit.date = "2026-09-30 12:00:00";
    edit.wallet_uuid = wallet;
    edit.category_uuid = category;
    edit.amount = "1";
    edit.comment = "правка ботом";
    wallet::PushResult edited = wallet_api->push_expense(edit, bot_token.access_token);
    CHECK(edited.status == wallet::PushResult::Status::AlreadyExists); // 403 operation_exists

    json account_push = {{"batch_id", uuidgen::v4()},
                         {"changes", {{"account", json::array({{{"uuid", uuidgen::v4()}, {"name", "Бот"}, {"comment", ""},
                                                                {"icon_uuid", icon}, {"is_archived", false},
                                                                {"system_kind", 0}, {"kind", 0}}})}}}};
    Http bot_account = request(e.api_url, "POST", "/sync/push", bot_token.access_token, account_push.dump());
    CHECK_EQ(bot_account.status, 403);
    CHECK_EQ(bot_account.body.value("error", std::string{}), std::string("only_operation"));
    Http bot_pull = request(e.api_url, "GET", "/sync/pull?ledger_seq=0", bot_token.access_token);
    CHECK_EQ(bot_pull.status, 403);

    ops = operations(e);
    REQUIRE(ops.size() == 1u);
    CHECK(contains(ops[0].value("amount", std::string{}), "350.5"));

    // --- /unlink: связка в google-auth удалена, токенов больше не выдают.
    b.handle(message(e, "/unlink", 5));
    CHECK_EQ(tg.sent.back(), std::string(bot::texts::kUnlinked));
    CHECK(auth_api->token(e.tg_user_id).status == auth::TokenResult::Status::NotLinked);
}
