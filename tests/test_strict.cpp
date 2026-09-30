// Строгий формат траты (parser/strict.hpp) и UUID v5 (util/uuid.hpp) —
// таблица разбора, основной артефакт шага 6 (org/docs/TODO_telegram_bot.md).
#include "parser/strict.hpp"

#include "test_framework.hpp"
#include "util/uuid.hpp"

namespace {

using parser::ParseError;

const parser::Names kNames{
    {"Продукты", "Кафе", "Кафе и рестораны", "Такси", "Ёлка", "Дом"},
    {"Тинькофф рубли", "Тинькофф евро", "Revolut", "Наличные"},
    {{"RUB", "₽"}, {"EUR", "€"}, {"USD", "$"}},
};
const parser::Now kNow{"2026-09-29", "14:05:00"};

parser::ParsedExpense ok(const std::string& text, const parser::Now& now = kNow) {
    auto r = parser::parse_strict(text, kNames, now);
    if (!r.ok) {
        ::testing::report_failure("parse_strict(\"" + text + "\") failed: " + parser::reason_name(r.error.reason),
                                  __FILE__, __LINE__);
    }
    return r.expense;
}

ParseError::Reason fails(const std::string& text, const parser::Now& now = kNow) {
    auto r = parser::parse_strict(text, kNames, now);
    if (r.ok) ::testing::report_failure("parse_strict(\"" + text + "\") should fail", __FILE__, __LINE__);
    return r.error.reason;
}

} // namespace

TEST(strict_minimal_expense_today) {
    auto e = ok("350 продукты");
    CHECK_EQ(e.amount, std::string("350"));
    CHECK_EQ(e.fraction_digits, 0);
    CHECK_EQ(e.category, std::string("продукты"));
    CHECK(e.wallet.empty());
    CHECK(e.currency_hint.empty());
    CHECK_EQ(e.datetime, std::string("2026-09-29 14:05:00"));
    CHECK(e.comment.empty());
}

TEST(strict_full_form_from_plan) {
    auto e = ok("1200,50 кафе @тинькофф рубли вчера // обед с командой");
    CHECK_EQ(e.amount, std::string("1200.5"));
    CHECK_EQ(e.fraction_digits, 1);
    CHECK_EQ(e.category, std::string("кафе"));
    CHECK_EQ(e.wallet, std::string("тинькофф рубли"));
    CHECK_EQ(e.datetime, std::string("2026-09-28 12:00:00"));
    CHECK_EQ(e.comment, std::string("обед с командой"));

    auto u = ok("99.9 USD такси @revolut 24.09");
    CHECK_EQ(u.amount, std::string("99.9"));
    CHECK_EQ(u.currency_hint, std::string("USD"));
    CHECK_EQ(u.category, std::string("такси"));
    CHECK_EQ(u.wallet, std::string("revolut"));
    CHECK_EQ(u.datetime, std::string("2026-09-24 12:00:00"));
}

TEST(strict_amount_separators_and_thousands) {
    CHECK_EQ(ok("1 200 кафе").amount, std::string("1200"));
    CHECK_EQ(ok("1 200 000,50 кафе").amount, std::string("1200000.5"));
    CHECK_EQ(ok("1\xC2\xA0" "200 кафе").amount, std::string("1200"));     // неразрывный пробел
    CHECK_EQ(ok("12\xE2\x80\xAF" "345 кафе").amount, std::string("12345")); // узкий неразрывный
    CHECK_EQ(ok("350,00 кафе").amount, std::string("350"));
    CHECK_EQ(ok("350,00 кафе").fraction_digits, 0);
    CHECK_EQ(ok("0,5 кафе").amount, std::string("0.5"));
    CHECK_EQ(ok("007 кафе").amount, std::string("7"));
    CHECK_EQ(ok("1.200 кафе").amount, std::string("1.2")); // один разделитель — всегда десятичный

    // Группа тысяч — ровно три цифры: "1 20" — сумма 1 и категория "20".
    auto e = ok("1 20 кафе");
    CHECK_EQ(e.amount, std::string("1"));
    CHECK_EQ(e.category, std::string("20"));
}

TEST(strict_amount_errors) {
    CHECK(fails("продукты 350") == ParseError::Reason::NoAmount);
    CHECK(fails("") == ParseError::Reason::NoAmount);
    CHECK(fails("   ") == ParseError::Reason::NoAmount);
    CHECK(fails("привет") == ParseError::Reason::NoAmount);
    CHECK(fails("кафе350") == ParseError::Reason::NoAmount);
    CHECK(fails("...привет") == ParseError::Reason::NoAmount);
    CHECK(fails("1,200.50 кафе") == ParseError::Reason::BadAmount);
    CHECK(fails("350. кафе") == ParseError::Reason::BadAmount);
    CHECK(fails(",5 кафе") == ParseError::Reason::BadAmount);
    CHECK(fails("-350 кафе") == ParseError::Reason::BadAmount);
    CHECK(fails("350кафе") == ParseError::Reason::BadAmount);
    CHECK(fails("1234567890123 кафе") == ParseError::Reason::BadAmount); // > numeric(20,8)
    CHECK(fails("1,123456789 кафе") == ParseError::Reason::BadAmount);
    CHECK(fails("0 кафе") == ParseError::Reason::ZeroAmount);
    CHECK(fails("0,00 кафе") == ParseError::Reason::ZeroAmount);
}

TEST(strict_currency_is_a_hint) {
    CHECK_EQ(ok("350₽ кафе").currency_hint, std::string("RUB"));
    CHECK_EQ(ok("$12 кафе").currency_hint, std::string("USD"));
    CHECK_EQ(ok("12€ кафе").currency_hint, std::string("EUR"));
    CHECK_EQ(ok("350 р кафе").currency_hint, std::string("RUB"));
    CHECK_EQ(ok("350 руб. кафе").currency_hint, std::string("RUB"));
    CHECK_EQ(ok("350 рублей кафе").currency_hint, std::string("RUB"));
    CHECK_EQ(ok("12 евро кафе").currency_hint, std::string("EUR"));
    CHECK_EQ(ok("12 eur кафе").currency_hint, std::string("EUR"));
    CHECK_EQ(ok("12 баксов кафе").currency_hint, std::string("USD"));
    CHECK_EQ(ok("1 200 ₽ кафе").currency_hint, std::string("RUB"));
    CHECK_EQ(ok("1 200₽ кафе").amount, std::string("1200"));
    // ISO-код без кошелька в этой валюте — всё равно валюта, не категория:
    // resolver ответит «нет кошелька в GBP».
    auto gbp = ok("10 gbp кафе");
    CHECK_EQ(gbp.currency_hint, std::string("GBP"));
    CHECK_EQ(gbp.category, std::string("кафе"));
    // Две валюты сразу — непонятно, какая.
    CHECK(fails("$12€ кафе") == ParseError::Reason::BadAmount);
}

TEST(strict_wallet_name_wins_over_currency_word) {
    // «евро» — часть имени кошелька «Тинькофф евро», валюты в сообщении нет.
    auto e = ok("350 кафе @тинькофф евро");
    CHECK_EQ(e.wallet, std::string("тинькофф евро"));
    CHECK(e.currency_hint.empty());
    CHECK(e.comment.empty());

    // Кошелёк неоднозначен (два «Тинькофф…») — resolver выберет по валюте.
    auto amb = ok("12 евро кафе @тинькофф");
    CHECK_EQ(amb.wallet, std::string("тинькофф"));
    CHECK_EQ(amb.currency_hint, std::string("EUR"));

    // Кошелька «Revolut евро» нет — «евро» остаётся подсказкой валюты.
    auto rev = ok("12 кафе @revolut евро");
    CHECK_EQ(rev.wallet, std::string("revolut"));
    CHECK_EQ(rev.currency_hint, std::string("EUR"));
}

TEST(strict_multiword_names_longest_match) {
    auto e = ok("2500 кафе и рестораны @тинькофф рубли");
    CHECK_EQ(e.category, std::string("кафе и рестораны"));
    CHECK_EQ(e.wallet, std::string("тинькофф рубли"));

    // Точное «Кафе», остальное — комментарий.
    auto c = ok("300 кафе с коллегами");
    CHECK_EQ(c.category, std::string("кафе"));
    CHECK_EQ(c.comment, std::string("с коллегами"));

    // Нет точного — самый длинный префикс.
    auto p = ok("300 кафе и рест");
    CHECK_EQ(p.category, std::string("кафе и рест"));
    auto pr = ok("300 прод молоко");
    CHECK_EQ(pr.category, std::string("прод"));
    CHECK_EQ(pr.comment, std::string("молоко"));

    // Префикс на границе слова ("кафе и") проигрывает точному «Кафе».
    auto pizza = ok("300 кафе и пицца");
    CHECK_EQ(pizza.category, std::string("кафе"));
    CHECK_EQ(pizza.comment, std::string("и пицца"));
    auto tail_and = ok("300 кафе и");
    CHECK_EQ(tail_and.category, std::string("кафе"));
    CHECK_EQ(tail_and.comment, std::string("и"));

    // Неизвестное слово — категория из одного слова, resolver предложит кнопки.
    auto u = ok("450 шаурма у метро");
    CHECK_EQ(u.category, std::string("шаурма"));
    CHECK_EQ(u.comment, std::string("у метро"));

    // ё = е, регистр не важен; в category — текст как в сообщении.
    CHECK_EQ(ok("100 ЕЛКА").category, std::string("ЕЛКА"));
    CHECK_EQ(ok("100 @ Наличные дом").wallet, std::string("Наличные"));
}

TEST(strict_word_boundary_prefix_without_shorter_exact) {
    // Короткого точного имени нет — префикс на границе слова всё равно берётся.
    parser::Names names{{"Кафе и рестораны Москва", "Такси"}, {"Тинькофф рубли", "Тинькофф евро"}, {}};
    auto e = parser::parse_strict("300 кафе и рестораны обед", names, kNow);
    REQUIRE(e.ok);
    CHECK_EQ(e.expense.category, std::string("кафе и рестораны"));
    CHECK_EQ(e.expense.comment, std::string("обед"));

    // Одно слово — префикс двух кошельков: выбор за resolver.
    auto w = parser::parse_strict("300 такси @тинькофф обратно", names, kNow);
    REQUIRE(w.ok);
    CHECK_EQ(w.expense.wallet, std::string("тинькофф"));
    CHECK_EQ(w.expense.comment, std::string("обратно"));
}

TEST(strict_category_optional) {
    auto e = ok("350");
    CHECK_EQ(e.amount, std::string("350"));
    CHECK(e.category.empty());
    auto w = ok("350 @revolut");
    CHECK(w.category.empty());
    CHECK_EQ(w.wallet, std::string("revolut"));
}

TEST(strict_dates) {
    CHECK_EQ(ok("1 кафе сегодня").datetime, std::string("2026-09-29 14:05:00"));
    CHECK_EQ(ok("1 кафе ВЧЕРА").datetime, std::string("2026-09-28 12:00:00"));
    CHECK_EQ(ok("1 кафе позавчера").datetime, std::string("2026-09-27 12:00:00"));
    CHECK_EQ(ok("1 кафе 29.09").datetime, std::string("2026-09-29 14:05:00"));
    CHECK_EQ(ok("1 кафе 1.9").datetime, std::string("2026-09-01 12:00:00"));
    CHECK_EQ(ok("1 кафе 15.03.2025").datetime, std::string("2025-03-15 12:00:00"));
    // Дата и кошелёк — в любом порядке, слова между ними — в комментарий.
    auto e = ok("1 кафе вчера @revolut");
    CHECK_EQ(e.wallet, std::string("revolut"));
    CHECK_EQ(e.datetime, std::string("2026-09-28 12:00:00"));

    // DD.MM позже сегодня — прошлый год.
    CHECK_EQ(ok("1 кафе 30.09").datetime, std::string("2025-09-30 12:00:00"));
    CHECK_EQ(ok("1 кафе 31.12", {"2026-01-02", "09:00:00"}).datetime, std::string("2025-12-31 12:00:00"));

    // Границы месяца и года для «вчера».
    CHECK_EQ(ok("1 кафе вчера", {"2026-03-01", "09:00:00"}).datetime, std::string("2026-02-28 12:00:00"));
    CHECK_EQ(ok("1 кафе вчера", {"2028-03-01", "09:00:00"}).datetime, std::string("2028-02-29 12:00:00"));
    CHECK_EQ(ok("1 кафе позавчера", {"2027-01-01", "09:00:00"}).datetime, std::string("2026-12-30 12:00:00"));

    // 29.02: високосный год есть, не високосный — ошибка.
    CHECK_EQ(ok("1 кафе 29.02", {"2028-03-10", "09:00:00"}).datetime, std::string("2028-02-29 12:00:00"));
    CHECK(fails("1 кафе 29.02", {"2026-03-10", "09:00:00"}) == ParseError::Reason::BadDate);

    CHECK(fails("1 кафе 31.02") == ParseError::Reason::BadDate);
    CHECK(fails("1 кафе 32.01") == ParseError::Reason::BadDate);
    CHECK(fails("1 кафе 10.13") == ParseError::Reason::BadDate);
    CHECK(fails("1 кафе 01.10.2026") == ParseError::Reason::FutureDate);
}

TEST(strict_comments) {
    CHECK_EQ(ok("350 продукты // молоко, хлеб").comment, std::string("молоко, хлеб"));
    CHECK_EQ(ok("350 продукты//молоко").comment, std::string("молоко"));
    CHECK_EQ(ok("350 продукты молоко // и хлеб").comment, std::string("молоко и хлеб"));
    // После "//" — как есть: даты и @ там не разбираются.
    auto e = ok("350 продукты // вчера @revolut");
    CHECK_EQ(e.comment, std::string("вчера @revolut"));
    CHECK(e.wallet.empty());
    CHECK_EQ(e.datetime, std::string("2026-09-29 14:05:00"));
    // Вторая дата — уже комментарий.
    CHECK_EQ(ok("1 кафе вчера сегодня").comment, std::string("сегодня"));
}

TEST(strict_reason_names) {
    CHECK_EQ(std::string(parser::reason_name(ParseError::Reason::NoAmount)), std::string("no_amount"));
    CHECK_EQ(std::string(parser::reason_name(ParseError::Reason::FutureDate)), std::string("future_date"));
}

TEST(uuid_v5_matches_rfc_vectors) {
    // Эталоны — Python uuid.uuid5.
    const std::string dns = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
    const std::string url = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
    CHECK_EQ(uuidgen::v5(dns, "python.org"), std::string("886313e1-3b8a-5372-9b90-0c9aee199e5d"));
    CHECK_EQ(uuidgen::v5(url, "тест"), std::string("1ac646cf-1525-5ca4-9776-037d81ca0394"));
    CHECK_EQ(uuidgen::v5(dns, "a"), uuidgen::v5(dns, "a"));
    CHECK(uuidgen::v5(dns, "a") != uuidgen::v5(dns, "b"));
    CHECK(uuidgen::looks_valid(uuidgen::v5(dns, "")));
    CHECK_THROWS(uuidgen::v5("not-a-uuid", "x"));
}
