// text.hpp (имена, суммы, даты) и util/tz.hpp.
#include "text.hpp"

#include <cstdlib>
#include <ctime>

#include "test_framework.hpp"
#include "util/tz.hpp"

namespace {
const std::string kNnbsp = "\xE2\x80\xAF"; // узкий неразрывный пробел — разделитель тысяч
const std::string kNbsp = "\xC2\xA0";      // перед символом валюты
const std::string kMinus = "\xE2\x88\x92";
} // namespace

TEST(normalize_name_lowercases_cyrillic_and_yo) {
    CHECK_EQ(text::normalize_name("  Тинькофф   Black "), std::string("тинькофф black"));
    CHECK_EQ(text::normalize_name("ЁЛКА ёлка"), std::string("елка елка"));
    CHECK_EQ(text::normalize_name("ПРОДУКТЫ"), text::normalize_name("продукты"));
    CHECK_EQ(text::normalize_name("Café"), std::string("café")); // прочие символы — как есть
}

TEST(resolve_name_exact_prefix_ambiguous_missing) {
    std::vector<text::Named> items = {{"1", "Тинькофф"}, {"2", "Тинькофф Black"}, {"3", "Revolut"}};

    auto exact = text::resolve_name("тинькофф", items);
    CHECK(exact.kind == text::Match::Kind::Found);
    CHECK_EQ(exact.found.uuid, std::string("1"));

    auto prefix = text::resolve_name("REV", items);
    CHECK(prefix.kind == text::Match::Kind::Found);
    CHECK_EQ(prefix.found.uuid, std::string("3"));

    auto ambiguous = text::resolve_name("тин", items);
    CHECK(ambiguous.kind == text::Match::Kind::Ambiguous);
    CHECK_EQ(ambiguous.candidates.size(), 2u);

    CHECK(text::resolve_name("сбер", items).kind == text::Match::Kind::NotFound);
    CHECK(text::resolve_name("   ", items).kind == text::Match::Kind::NotFound);
}

TEST(format_amount_groups_pads_and_signs) {
    CHECK_EQ(text::format_amount("12345.6", "2", "₽"), "12" + kNnbsp + "345,60" + kNbsp + "₽");
    CHECK_EQ(text::format_amount("-5", "2", "$"), kMinus + "5,00" + kNbsp + "$");
    CHECK_EQ(text::format_amount("1000000", "0", ""), "1" + kNnbsp + "000" + kNnbsp + "000");
    CHECK_EQ(text::format_amount("0.50", "2", "€"), "0,50" + kNbsp + "€");
    CHECK_EQ(text::format_amount("+100", "2", ""), std::string("100,00"));
    // Лишние значащие знаки не округляются (это деньги), хвостовые нули — срезаются.
    CHECK_EQ(text::format_amount("1.23456000", "2", ""), std::string("1,23456"));
    CHECK_EQ(text::format_amount("007.10", "2", ""), std::string("7,10"));
    // Мусор — как есть.
    CHECK_EQ(text::format_amount("abc", "2", "₽"), std::string("abc"));
    CHECK_EQ(text::format_amount("", "2", "₽"), std::string(""));
}

TEST(format_day_month_variants) {
    CHECK_EQ(text::format_day_month("2026-09-28T12:00:00"), std::string("28.09"));
    CHECK_EQ(text::format_day_month("2026-01-05 00:00:00"), std::string("05.01"));
    CHECK_EQ(text::format_day_month("2026-12-31"), std::string("31.12"));
    CHECK_EQ(text::format_day_month("вчера"), std::string("вчера"));
}

TEST(truncate_utf8_does_not_split_characters) {
    std::string s = "абв"; // по 2 байта
    CHECK_EQ(text::truncate_utf8(s, 10), s);
    CHECK_EQ(text::truncate_utf8(s, 3), std::string("а") + "\xE2\x80\xA6"); // 3-й байт — середина «б»
    CHECK_EQ(text::truncate_utf8(s, 4), std::string("аб") + "\xE2\x80\xA6");
}

TEST(tz_names_are_validated_against_tzdata) {
    CHECK(tz::is_valid_name("Europe/Moscow"));
    CHECK(tz::is_valid_name("UTC"));
    CHECK(tz::is_valid_name("America/Argentina/Buenos_Aires"));
    CHECK(!tz::is_valid_name("Europe"));          // каталог, не пояс
    CHECK(!tz::is_valid_name("Mars/Olympus"));    // нет такого файла
    CHECK(!tz::is_valid_name("../../etc/passwd")); // выход из каталога
    CHECK(!tz::is_valid_name("/etc/localtime"));
    CHECK(!tz::is_valid_name("Europe/Moscow;rm"));
    CHECK(!tz::is_valid_name(""));
}

TEST(tz_local_date_crosses_midnight_by_zone) {
    // 2026-09-21 22:30 UTC: в Москве (UTC+3) уже 22-е, в Нью-Йорке ещё 21-е.
    std::time_t t = 1790029800;
    CHECK_EQ(tz::local_date("UTC", t), std::string("2026-09-21"));
    CHECK_EQ(tz::local_date("Europe/Moscow", t), std::string("2026-09-22"));
    CHECK_EQ(tz::local_date("America/New_York", t), std::string("2026-09-21"));
    CHECK_EQ(tz::local_date("Europe/Moscow", t, 1), std::string("2026-09-23"));
    CHECK_EQ(tz::local_time("UTC", t), std::string("22:30"));
    CHECK_EQ(tz::local_time("Europe/Moscow", t), std::string("01:30"));
    CHECK_EQ(tz::local_time("Europe/Podgorica", t), std::string("00:30")); // CEST, UTC+2
    CHECK_EQ(tz::local_time("Asia/Kolkata", t), std::string("04:00"));      // UTC+5:30
    // Подмена TZ не протекает наружу.
    CHECK(std::getenv("TZ") == nullptr);
}
