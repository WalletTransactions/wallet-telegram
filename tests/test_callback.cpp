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
