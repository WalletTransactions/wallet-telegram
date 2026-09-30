// Resolver (resolver.hpp) — таблица выбора кошелька и категории из
// org/docs/TODO_telegram_bot.md, §3.2 и «Подшаг 2 подробно».
#include "resolver.hpp"

#include "test_framework.hpp"

namespace {

using resolver::CategoryPick;
using resolver::WalletPick;

const resolver::Accounts kAccounts{
    {
        {"t-rub", "Тинькофф рубли", "RUB", "₽", 2},
        {"t-eur", "Тинькофф евро", "EUR", "€", 2},
        {"rev", "Revolut", "USD", "$", 2},
        {"cash", "Наличные", "RUB", "₽", 2},
    },
    {
        {"c-cafe", "Кафе"},
        {"c-rest", "Кафе и рестораны"},
        {"c-food", "Продукты"},
        {"c-taxi", "Такси"},
    },
};

parser::ParsedExpense expense(const std::string& category, const std::string& wallet = "",
                              const std::string& hint = "") {
    parser::ParsedExpense e;
    e.amount = "350";
    e.category = category;
    e.wallet = wallet;
    e.currency_hint = hint;
    e.datetime = "2026-09-29 14:05:00";
    return e;
}

CategoryPick category(const std::string& text) { return resolver::resolve(expense(text), kAccounts, "").category; }

WalletPick wallet(const std::string& wallet, const std::string& hint = "", const std::string& default_uuid = "",
                  const resolver::Accounts& accounts = kAccounts) {
    return resolver::resolve(expense("кафе", wallet, hint), accounts, default_uuid).wallet;
}

template <typename T>
std::vector<std::string> uuids(const std::vector<T>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.uuid);
    return out;
}

using Uuids = std::vector<std::string>;

} // namespace

TEST(resolver_category_found_exact_and_by_unique_prefix) {
    auto exact = category("ПРОДУКТЫ");
    CHECK(exact.kind == CategoryPick::Kind::Found);
    CHECK_EQ(exact.found.uuid, std::string("c-food"));
    // «кафе» — точное имя, хоть и префикс «Кафе и рестораны».
    CHECK_EQ(category("кафе").found.uuid, std::string("c-cafe"));
    CHECK_EQ(category("так").found.uuid, std::string("c-taxi"));
    CHECK_EQ(category("кафе и рест").found.uuid, std::string("c-rest"));
}

TEST(resolver_category_ask) {
    auto amb = category("каф");
    CHECK(amb.kind == CategoryPick::Kind::Ask);
    CHECK(amb.why == CategoryPick::Why::Ambiguous);
    CHECK(uuids(amb.candidates) == (Uuids{"c-cafe", "c-rest"}));

    // Не префикс, а середина имени.
    auto mid = category("рест");
    CHECK(mid.why == CategoryPick::Why::NotFound);
    CHECK(uuids(mid.candidates) == (Uuids{"c-rest"}));

    auto none = category("шаурма");
    CHECK(none.kind == CategoryPick::Kind::Ask);
    CHECK(none.why == CategoryPick::Why::NotFound);
    CHECK(none.candidates.empty());

    auto missing = category("");
    CHECK(missing.why == CategoryPick::Why::Missing);
    CHECK(uuids(missing.candidates) == (Uuids{"c-cafe", "c-rest", "c-food", "c-taxi"}));
}

TEST(resolver_category_candidates_limited) {
    resolver::Accounts many = kAccounts;
    for (int i = 0; i < 5; ++i) many.categories.push_back({"k-" + std::to_string(i), "Кино " + std::to_string(i)});
    CHECK_EQ(resolver::resolve(expense(""), many, "").category.candidates.size(), resolver::kMaxCandidates);
    CHECK_EQ(resolver::resolve(expense("кино"), many, "").category.candidates.size(), 5u);
}

// Кошелёк указан однозначно (таблица §3.2, строки 1–2).
TEST(resolver_named_wallet_and_currency) {
    auto plain = wallet("revolut");
    CHECK(plain.kind == WalletPick::Kind::Found);
    CHECK_EQ(plain.found.uuid, std::string("rev"));
    CHECK(!plain.by_currency);
    CHECK_EQ(wallet("revolut", "USD").found.uuid, std::string("rev"));
    // Точное имя «Тинькофф евро» — без вопросов.
    CHECK_EQ(wallet("тинькофф евро").found.uuid, std::string("t-eur"));

    // Другая валюта — не отказ, а вопрос: кошельки этой валюты и «всё равно».
    auto other = wallet("revolut", "EUR");
    CHECK(other.kind == WalletPick::Kind::Ask);
    CHECK(other.why == WalletPick::Why::CurrencyMismatch);
    CHECK(uuids(other.candidates) == (Uuids{"t-eur"}));
    REQUIRE(other.force.has_value());
    CHECK_EQ(other.force->uuid, std::string("rev"));

    // Кошельков этой валюты нет — остаётся только «всё равно».
    auto gbp = wallet("revolut", "GBP");
    CHECK(gbp.why == WalletPick::Why::CurrencyMismatch);
    CHECK(gbp.candidates.empty());
    CHECK(gbp.force.has_value());
}

// Кошелёк неоднозначен или не найден (строки 3–4 и края).
TEST(resolver_ambiguous_and_unknown_wallet) {
    auto eur = wallet("тинькофф", "EUR");
    CHECK(eur.kind == WalletPick::Kind::Found);
    CHECK_EQ(eur.found.uuid, std::string("t-eur"));
    CHECK(eur.by_currency);

    auto amb = wallet("тинькофф");
    CHECK(amb.why == WalletPick::Why::Ambiguous);
    CHECK(uuids(amb.candidates) == (Uuids{"t-rub", "t-eur"}));
    // Среди кандидатов нет этой валюты — спрашиваем обо всех.
    CHECK(uuids(wallet("тинькофф", "USD").candidates) == (Uuids{"t-rub", "t-eur"}));

    auto unknown = wallet("сбер");
    CHECK(unknown.why == WalletPick::Why::NotFound);
    CHECK_EQ(unknown.candidates.size(), 4u);
    CHECK(uuids(wallet("сбер", "RUB").candidates) == (Uuids{"t-rub", "cash"}));
    CHECK_EQ(wallet("сбер", "GBP").candidates.size(), 4u);
}

// Кошелёк не указан (строки 5–6).
TEST(resolver_default_wallet) {
    CHECK_EQ(wallet("", "", "rev").found.uuid, std::string("rev"));

    auto none = wallet("");
    CHECK(none.why == WalletPick::Why::NotGiven);
    CHECK_EQ(none.candidates.size(), 4u);
    CHECK(none.offer_remember);
    // Кошелёк по умолчанию удалили — как будто не задан.
    CHECK(wallet("", "", "gone").why == WalletPick::Why::NotGiven);

    auto same = wallet("", "RUB", "t-rub");
    CHECK_EQ(same.found.uuid, std::string("t-rub"));
    CHECK(!same.by_currency);

    auto by_currency = wallet("", "EUR", "t-rub");
    CHECK_EQ(by_currency.found.uuid, std::string("t-eur"));
    CHECK(by_currency.by_currency);

    auto two_rub = wallet("", "RUB", "rev");
    CHECK(two_rub.why == WalletPick::Why::NotGiven);
    CHECK(uuids(two_rub.candidates) == (Uuids{"t-rub", "cash"}));
    CHECK(!two_rub.offer_remember);
    CHECK(wallet("", "RUB").offer_remember);

    auto gbp = wallet("", "GBP", "rev");
    CHECK(gbp.kind == WalletPick::Kind::Reject);
    CHECK(gbp.why == WalletPick::Why::NoWalletInCurrency);
}

TEST(resolver_single_and_no_wallets) {
    resolver::Accounts one = kAccounts;
    one.wallets = {kAccounts.wallets[0]};
    CHECK_EQ(wallet("", "", "", one).found.uuid, std::string("t-rub"));
    CHECK(wallet("", "EUR", "", one).why == WalletPick::Why::NoWalletInCurrency);

    resolver::Accounts empty = kAccounts;
    empty.wallets.clear();
    auto none = wallet("", "", "", empty);
    CHECK(none.kind == WalletPick::Kind::Reject);
    CHECK(none.why == WalletPick::Why::NoWallets);
}

TEST(resolver_fits_precision) {
    resolver::Wallet rub{"r", "R", "RUB", "₽", 2};
    resolver::Wallet jpy{"j", "J", "JPY", "¥", 0};
    parser::ParsedExpense e = expense("кафе");
    e.fraction_digits = 2;
    CHECK(resolver::fits_precision(e, rub));
    CHECK(!resolver::fits_precision(e, jpy));
    e.fraction_digits = 3;
    CHECK(!resolver::fits_precision(e, rub));
    e.fraction_digits = 0;
    CHECK(resolver::fits_precision(e, jpy));
}

TEST(resolver_names_for_parser_and_end_to_end) {
    parser::Names names = resolver::names_for_parser(kAccounts);
    CHECK_EQ(names.categories.size(), 4u);
    CHECK_EQ(names.wallets.size(), 4u);
    REQUIRE(names.currencies.size() == 3u); // RUB, EUR, USD — без повторов
    CHECK_EQ(names.currencies[0].iso_code, std::string("RUB"));
    CHECK_EQ(names.currencies[0].symbol, std::string("₽"));

    auto parsed = parser::parse_strict("12 евро кафе и рест @тинькофф вчера", names, {"2026-09-29", "14:05:00"});
    REQUIRE(parsed.ok);
    auto r = resolver::resolve(parsed.expense, kAccounts, "cash");
    CHECK_EQ(r.category.found.uuid, std::string("c-rest"));
    CHECK_EQ(r.wallet.found.uuid, std::string("t-eur"));
    CHECK(r.wallet.by_currency);
}
