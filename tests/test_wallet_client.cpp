// Клиент wallet-api (wallet_client.hpp): тело POST /sync/push и разбор
// ответа — таблица из org/docs/TODO_telegram_bot.md, «Подшаг 4 подробно».
#include "wallet_client.hpp"

#include "test_framework.hpp"

using nlohmann::json;
using wallet::PushResult;

namespace {

wallet::Expense expense() {
    wallet::Expense e;
    e.batch_id = "b-1";
    e.op_uuid = "op-1";
    e.date = "2026-09-30 14:05:00";
    e.wallet_uuid = "w-1";
    e.category_uuid = "c-1";
    e.amount = "350.5";
    e.comment = "молоко";
    return e;
}

PushResult::Status status_of(int http, const std::string& body) { return wallet::parse_push_response(http, body).status; }

} // namespace

TEST(wallet_push_body_shape) {
    json b = wallet::push_body(expense());
    CHECK_EQ(b.at("batch_id").get<std::string>(), std::string("b-1"));
    CHECK_EQ(b.at("device_name").get<std::string>(), std::string("Telegram"));
    const json& changes = b.at("changes");
    REQUIRE(changes.size() == 1u); // только operation — иначе wallet-api ответит 403 only_operation
    const json& ops = changes.at("operation");
    REQUIRE(ops.size() == 1u);
    const json& op = ops[0];
    CHECK_EQ(op.at("uuid").get<std::string>(), std::string("op-1"));
    CHECK_EQ(op.at("date").get<std::string>(), std::string("2026-09-30 14:05:00"));
    // Расход: из кошелька в категорию.
    CHECK_EQ(op.at("acc_fr_uuid").get<std::string>(), std::string("w-1"));
    CHECK_EQ(op.at("acc_to_uuid").get<std::string>(), std::string("c-1"));
    CHECK(op.at("amount").is_string()); // numeric на сервере — строкой, без double
    CHECK_EQ(op.at("amount").get<std::string>(), std::string("350.5"));
    CHECK_EQ(op.at("comment").get<std::string>(), std::string("молоко"));
    CHECK(!op.contains("deleted"));
    CHECK_EQ(op.size(), 6u);
}

TEST(wallet_parse_push_ok) {
    auto ok = wallet::parse_push_response(200, R"({"seq": 42, "conflicts": []})");
    CHECK(ok.status == PushResult::Status::Ok);
    CHECK_EQ(ok.seq, static_cast<std::int64_t>(42));
    CHECK_EQ(ok.http_status, 200);
    // 200 без seq или не JSON — неизвестно, записано ли: как недоступность.
    CHECK(status_of(200, "not json") == PushResult::Status::Unavailable);
    CHECK(status_of(200, R"({"conflicts": []})") == PushResult::Status::Unavailable);
    CHECK(status_of(200, R"({"seq": "42"})") == PushResult::Status::Unavailable);
}

TEST(wallet_parse_push_errors_table) {
    using S = PushResult::Status;
    CHECK(status_of(401, R"({"error": "unauthenticated"})") == S::Unauthorized);
    CHECK(status_of(401, "") == S::Unauthorized);

    auto exists = wallet::parse_push_response(403, R"({"error": "operation_exists", "table": "operation", "uuid": "x"})");
    CHECK(exists.status == S::AlreadyExists);
    CHECK_EQ(exists.error, std::string("operation_exists"));
    CHECK(status_of(403, R"({"error": "reader_cannot_write"})") == S::Reader);
    CHECK(status_of(403, R"({"error": "нужен scope wallet:write"})") == S::Rejected);
    CHECK(status_of(403, R"({"error": "only_operation"})") == S::Rejected);
    CHECK(status_of(403, R"({"error": "delete_not_allowed"})") == S::Rejected);
    CHECK(status_of(403, "") == S::Rejected);

    // Составной FK операции — без table/uuid, всё равно MissingReference.
    CHECK(status_of(409, R"({"error": "missing_reference", "detail": "..."})") == S::MissingReference);
    CHECK(status_of(409, R"({"error": "duplicate_key", "constraint": "x"})") == S::Rejected);
    CHECK(status_of(409, R"({"conflicts": []})") == S::Rejected);
    CHECK(status_of(400, R"({"error": "invalid_row"})") == S::Rejected);

    auto down = wallet::parse_push_response(503, "");
    CHECK(down.status == S::Unavailable);
    CHECK_EQ(down.error, std::string("HTTP 503"));
    CHECK(status_of(500, R"({"error": "boom"})") == S::Unavailable);
    CHECK(status_of(0, "") == S::Unavailable);
}

TEST(wallet_status_names) {
    CHECK_EQ(std::string(wallet::status_name(PushResult::Status::AlreadyExists)), std::string("already_exists"));
    CHECK_EQ(std::string(wallet::status_name(PushResult::Status::MissingReference)), std::string("missing_reference"));
}
