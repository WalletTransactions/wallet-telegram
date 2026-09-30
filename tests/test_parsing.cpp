// Чистые функции разбора: команды из текста сообщения, код связывания,
// ответ getUpdates.
#include "bot.hpp"
#include "nlohmann/json.hpp"
#include "telegram_client.hpp"
#include "test_framework.hpp"

using nlohmann::json;

TEST(parse_command_plain_and_with_args) {
    auto c = bot::parse_command("/start abc_DEF-123", "WalletTransaction_bot");
    CHECK_EQ(c.name, std::string("start"));
    CHECK_EQ(c.args, std::string("abc_DEF-123"));

    auto h = bot::parse_command("  /Help  ", "WalletTransaction_bot");
    CHECK_EQ(h.name, std::string("help"));
    CHECK_EQ(h.args, std::string(""));
}

TEST(parse_command_accepts_own_mention_case_insensitive) {
    auto c = bot::parse_command("/start@wallettransaction_BOT code", "WalletTransaction_bot");
    CHECK_EQ(c.name, std::string("start"));
    CHECK_EQ(c.args, std::string("code"));
}

TEST(parse_command_rejects_other_bot_mention) {
    auto c = bot::parse_command("/start@other_bot code", "WalletTransaction_bot");
    CHECK(c.name.empty());
}

TEST(parse_command_not_a_command) {
    CHECK(bot::parse_command("350 продукты", "b").name.empty());
    CHECK(bot::parse_command("/", "b").name.empty());
    CHECK(bot::parse_command("", "b").name.empty());
    CHECK(bot::parse_command("/@b", "b").name.empty());
}

TEST(parse_command_args_keep_inner_spaces_and_unicode) {
    auto c = bot::parse_command("/default Тинькофф Black\n", "");
    CHECK_EQ(c.name, std::string("default"));
    CHECK_EQ(c.args, std::string("Тинькофф Black"));
}

TEST(looks_like_link_code_bounds_and_alphabet) {
    CHECK(bot::looks_like_link_code("abcdefghijklmnop"));                  // 16
    CHECK(bot::looks_like_link_code(std::string(64, 'A')));                // 64
    CHECK(bot::looks_like_link_code("Ab-_09Ab-_09Ab-_09Ab-_09Ab-_09Ab")); // 32, как у google-auth
    CHECK(!bot::looks_like_link_code("short"));
    CHECK(!bot::looks_like_link_code(std::string(65, 'A')));
    CHECK(!bot::looks_like_link_code("abcdefghijklmnop="));
    CHECK(!bot::looks_like_link_code("abcdefgh ijklmnop"));
    CHECK(!bot::looks_like_link_code("кодкодкодкодкодкод"));
}

TEST(parse_updates_private_text_message) {
    json body = json::parse(R"({
      "ok": true,
      "result": [{
        "update_id": 1000,
        "message": {
          "message_id": 5,
          "from": {"id": 211959201, "is_bot": false, "username": "ivan"},
          "chat": {"id": 211959201, "type": "private"},
          "date": 1759000000,
          "text": "/start abc"
        }
      }]
    })");
    auto updates = telegram::parse_updates(body);
    REQUIRE(updates.size() == 1u);
    const auto& u = updates[0];
    CHECK_EQ(u.update_id, static_cast<std::int64_t>(1000));
    CHECK(u.has_message);
    CHECK_EQ(u.chat_id, static_cast<std::int64_t>(211959201));
    CHECK_EQ(u.chat_type, std::string("private"));
    CHECK_EQ(u.from_id, static_cast<std::int64_t>(211959201));
    CHECK_EQ(u.from_username, std::string("ivan"));
    CHECK_EQ(u.text, std::string("/start abc"));
    CHECK_EQ(u.message_id, static_cast<std::int64_t>(5));
    CHECK(!u.has_callback);
}

TEST(parse_updates_callback_query) {
    json body = json::parse(R"({
      "ok": true,
      "result": [{
        "update_id": 1001,
        "callback_query": {
          "id": "4382bfdwdsb323b2d9",
          "from": {"id": 211959201, "is_bot": false, "username": "ivan"},
          "message": {
            "message_id": 77,
            "from": {"id": 8000000001, "is_bot": true, "username": "WalletTransaction_bot"},
            "chat": {"id": 211959201, "type": "private"},
            "date": 1759000000,
            "text": "Какая категория?"
          },
          "chat_instance": "-123",
          "data": "1000:c:1"
        }
      }]
    })");
    auto updates = telegram::parse_updates(body);
    REQUIRE(updates.size() == 1u);
    const auto& u = updates[0];
    CHECK(u.has_callback);
    CHECK(!u.has_message);
    CHECK_EQ(u.callback_id, std::string("4382bfdwdsb323b2d9"));
    CHECK_EQ(u.callback_data, std::string("1000:c:1"));
    CHECK_EQ(u.message_id, static_cast<std::int64_t>(77));
    CHECK_EQ(u.chat_id, static_cast<std::int64_t>(211959201));
    CHECK_EQ(u.chat_type, std::string("private"));
    // Нажал пользователь, а не автор сообщения (бот).
    CHECK_EQ(u.from_id, static_cast<std::int64_t>(211959201));
    CHECK_EQ(u.from_username, std::string("ivan"));
    CHECK(u.text.empty());
}

TEST(parse_updates_callback_without_message) {
    json body = json::parse(R"({"ok": true, "result": [
      {"update_id": 3, "callback_query": {"id": "q", "from": {"id": 42}, "data": "1:x"}}
    ]})");
    auto updates = telegram::parse_updates(body);
    REQUIRE(updates.size() == 1u);
    CHECK(updates[0].has_callback);
    CHECK_EQ(updates[0].message_id, static_cast<std::int64_t>(0));
    CHECK_EQ(updates[0].chat_id, static_cast<std::int64_t>(42));
    CHECK(updates[0].chat_type.empty());
}

TEST(keyboard_json_shape_and_limits) {
    telegram::Keyboard kb = {{{"Кафе", "1:c:0"}, {"Кафе и рестораны", "1:c:1"}}, {{"✕", "1:x"}}};
    json j = telegram::keyboard_json(kb);
    REQUIRE(j["inline_keyboard"].size() == 2u);
    CHECK_EQ(j["inline_keyboard"][0].size(), 2u);
    CHECK_EQ(j["inline_keyboard"][0][1]["text"].get<std::string>(), std::string("Кафе и рестораны"));
    CHECK_EQ(j["inline_keyboard"][0][1]["callback_data"].get<std::string>(), std::string("1:c:1"));
    CHECK_EQ(j["inline_keyboard"][1][0]["callback_data"].get<std::string>(), std::string("1:x"));

    // Пустая клавиатура — пустой inline_keyboard (edit_message убирает кнопки).
    CHECK(telegram::keyboard_json({})["inline_keyboard"].empty());

    // Длинное имя режется по границе UTF-8, с многоточием.
    std::string long_name;
    for (int i = 0; i < 40; ++i) long_name += "ж"; // 80 байт
    std::string cut = telegram::keyboard_json({{{long_name, "d"}}})["inline_keyboard"][0][0]["text"];
    CHECK(cut.size() <= telegram::kMaxButtonTextBytes + 3);
    CHECK(cut.compare(cut.size() - 3, 3, "\xE2\x80\xA6") == 0);

    CHECK_THROWS(telegram::keyboard_json({{{"a", ""}}}));
    CHECK_THROWS(telegram::keyboard_json({{{"a", std::string(65, 'x')}}}));
    telegram::keyboard_json({{{"a", std::string(64, 'x')}}}); // ровно 64 — можно
}

TEST(parse_updates_non_message_and_sticker) {
    json body = json::parse(R"({
      "ok": true,
      "result": [
        {"update_id": 1, "inline_query": {"id": "x"}},
        {"update_id": 2, "message": {"chat": {"id": -100, "type": "group"}, "from": {"id": 7}, "sticker": {}}}
      ]
    })");
    auto updates = telegram::parse_updates(body);
    REQUIRE(updates.size() == 2u);
    CHECK(!updates[0].has_message);
    CHECK(!updates[0].has_callback);
    CHECK_EQ(updates[0].update_id, static_cast<std::int64_t>(1));
    CHECK(updates[1].has_message);
    CHECK_EQ(updates[1].chat_type, std::string("group"));
    CHECK_EQ(updates[1].chat_id, static_cast<std::int64_t>(-100));
    CHECK(updates[1].text.empty());
}

TEST(parse_updates_rejects_not_ok) {
    CHECK_THROWS(telegram::parse_updates(json::parse(R"({"ok": false, "description": "Unauthorized"})")));
    CHECK_THROWS(telegram::parse_updates(json::parse(R"({"ok": true, "result": {}})")));
    CHECK_THROWS(telegram::parse_updates(json::array()));
}
