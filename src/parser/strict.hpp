// Строгий формат траты (org/docs/TODO_telegram_bot.md, §3.1):
//
//   <сумма> [<валюта>] <категория> [@<кошелёк>] [<дата>] [// <комментарий>]
//
// Чистая функция: текст, имена пользователя и «сейчас» в его поясе →
// разобранные поля. Без сети, базы и часов — таблица тестов в
// tests/test_strict.cpp.
//
// Парсер только режет текст на поля. Какой это счёт (uuid), что делать с
// неоднозначностью и подходит ли валюта кошельку, решает resolver (§3.2).
// Имена ему всё же нужны: без «//» граница между многословной категорией и
// комментарием видна только по списку имён (самое длинное совпадение).
#pragma once

#include <string>
#include <vector>

namespace parser {

struct Currency {
    std::string iso_code; // "RUB"
    std::string symbol;   // "₽", может быть пустым
};

struct Names {
    std::vector<std::string> categories;
    std::vector<std::string> wallets;
    // Валюты кошельков пользователя: по ним узнаются ISO-коды и символы
    // помимо встроенных синонимов (руб, евро, $ …).
    std::vector<Currency> currencies;
};

struct Now {
    std::string date; // "YYYY-MM-DD" в поясе пользователя
    std::string time; // "HH:MM:SS" там же
};

struct ParsedExpense {
    // Десятичная строка с точкой, без ведущих нулей и хвостовых нулей
    // дробной части: "1200.5", "350". Всегда > 0.
    std::string amount;
    // Число знаков после точки в amount — resolver сверяет его с precision
    // валюты выбранного кошелька.
    int fraction_digits = 0;
    // ISO-код валюты из сообщения, пусто — не указана. Не поле операции, а
    // подсказка для выбора кошелька (§3.2).
    std::string currency_hint;
    // Текст категории как в сообщении (пусто — не указана, resolver
    // спросит кнопками).
    std::string category;
    // Текст после '@' (пусто — не указан).
    std::string wallet;
    // Дата операции "YYYY-MM-DD HH:MM:SS" (timestamp без TZ): сегодня —
    // текущее время, прошедший день — 12:00.
    std::string datetime;
    std::string comment;
};

struct ParseError {
    enum class Reason {
        NoAmount,   // сообщение не начинается с суммы — это не трата
        BadAmount,  // "1,200.50", "350кафе", слишком длинная сумма
        ZeroAmount, // "0", "0,00"
        BadDate,    // "31.02", "32.13"
        FutureDate, // дата позже сегодняшней
    };
    Reason reason = Reason::NoAmount;
};

// Имя причины для метрики tg_parse_fail_total{reason}.
const char* reason_name(ParseError::Reason reason);

struct Result {
    bool ok = false;
    ParsedExpense expense; // при ok
    ParseError error;      // при !ok
};

Result parse_strict(const std::string& text, const Names& names, const Now& now);

} // namespace parser
