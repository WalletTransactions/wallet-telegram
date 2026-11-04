// callback_data кнопок карточки (callback.hpp): формат и строгий разбор —
// данные приходят от клиента.
#include "callback.hpp"

#include "telegram_client.hpp"
#include "test_framework.hpp"

using callback::Kind;

TEST(callback_make_and_parse_roundtrip) {
    CHECK_EQ(callback::make("1000", Kind::PickCategory, 3), std::string("1000:c:3"));
    CHECK_EQ(callback::make("1000", Kind::CategoryPage, 0), std::string("1000:cp:0"));
    CHECK_EQ(callback::make("1000", Kind::Cancel), std::string("1000:x"));
    CHECK_EQ(callback::make("1000", Kind::Save), std::string("1000:ok"));
    CHECK_EQ(callback::make("1000", Kind::Cancel, 5), std::string("1000:x")); // число только у c/cp/w

    for (Kind k : {Kind::PickCategory, Kind::CategoryPage, Kind::PickWallet, Kind::EditCategory, Kind::EditWallet,
                   Kind::MakeDefault, Kind::Cancel, Kind::Save}) {
        auto a = callback::parse(callback::make("42", k, 7));
        REQUIRE(a.has_value());
        CHECK_EQ(a->draft_id, std::string("42"));
        CHECK(a->kind == k);
    }
    auto w = callback::parse("42:w:12");
    REQUIRE(w.has_value());
    CHECK_EQ(w->number, 12);
    CHECK_EQ(std::string(callback::kind_name(Kind::MakeDefault)), std::string("wd"));
}

TEST(callback_longest_fits_bot_api_limit) {
    std::string longest = callback::make("18446744073709551615", Kind::CategoryPage, 999);
    CHECK_EQ(longest.size(), 27u);
    CHECK(longest.size() <= telegram::kMaxCallbackDataBytes);
}

TEST(callback_parse_rejects_garbage) {
    for (const char* bad : {"", "x", ":x", "12", "12:", "12:zz", "12:c", "12:c:", "12:c:abc", "12:c:1000",
                            "12:c:-1", "12:x:1", "12:c:1:2", "ab:x", "123456789012345678901:x", "12:X"}) {
        if (callback::parse(bad).has_value()) {
            ::testing::report_failure(std::string("parse(\"") + bad + "\") should fail", __FILE__, __LINE__);
        }
    }
}

TEST(callback_ai_strict_format) {
    using callback::AiKind;
    auto sel = callback::parse_ai("ai:sel:claude");
    REQUIRE(sel.has_value());
    CHECK(sel->kind == AiKind::Select);
    CHECK_EQ(sel->provider, std::string("claude"));
    CHECK(callback::parse_ai("ai:consent:claude")->kind == AiKind::Consent);
    CHECK(callback::parse_ai("ai:revoke:claude")->kind == AiKind::Revoke);
    auto back = callback::parse_ai("ai:back");
    REQUIRE(back.has_value());
    CHECK(back->kind == AiKind::Back);
    CHECK(back->provider.empty());

    // Туда-обратно.
    CHECK_EQ(callback::make_ai(AiKind::Select, "lmstudio"), std::string("ai:sel:lmstudio"));
    CHECK_EQ(callback::make_ai(AiKind::Back), std::string("ai:back"));
    CHECK(callback::parse_ai(callback::make_ai(AiKind::Revoke, "claude"))->kind == AiKind::Revoke);

    // Подделки и мусор.
    CHECK(!callback::parse_ai("ai:sel:../../admin").has_value());
    CHECK(!callback::parse_ai("ai:sel:").has_value());          // пустой провайдер
    CHECK(!callback::parse_ai("ai:sel:claude:x").has_value());  // лишние поля
    CHECK(!callback::parse_ai("ai:sel:Claude").has_value());    // только [a-z]
    CHECK(!callback::parse_ai("ai:sel:abcdefghijklmnopq").has_value()); // 17 букв
    CHECK(callback::parse_ai("ai:sel:abcdefghijklmnop").has_value());   // 16 — можно
    CHECK(!callback::parse_ai("ai:drop:claude").has_value());   // неизвестное действие
    CHECK(!callback::parse_ai("ai:back:x").has_value());
    CHECK(!callback::parse_ai("ai:").has_value());
    CHECK(!callback::parse_ai("30:ok").has_value());            // кнопка черновика — не ai
    CHECK(callback::is_ai("ai:whatever"));
    CHECK(!callback::is_ai("30:ok"));
    // Кнопки черновика не путаются с ai.
    CHECK(!callback::parse("ai:back").has_value());
}
