#include "chat_routing.hpp"

#include <array>
#include <cctype>

#include "expense/text.hpp"

namespace chat_routing {

namespace {

// Список намеренно короткий (D1): ложное «вопрос» для траты хуже, чем лишний
// шаг через not_expense. «Потратил» сюда не входит — «потратил 300 на кофе»
// трата.
constexpr std::array<const char*, 15> kQuestionWords = {
    "сколько", "какой", "какая", "какое", "какие", "каких", "где",    "когда",
    "что",     "чем",   "почему", "покажи", "сравни", "итого", "баланс",
};

// Байт обрывает слово: ASCII-пробел или ASCII-пунктуация, кроме дефиса.
// Байты UTF-8 (кириллица) — всегда часть слова.
bool ends_word(unsigned char c) {
    if (c >= 0x80) return false;
    if (c == '-') return false;
    return std::isspace(c) != 0 || std::ispunct(c) != 0;
}

} // namespace

std::string first_word(const std::string& text) {
    std::string s = text::normalize_name(text);
    std::size_t end = 0;
    while (end < s.size() && !ends_word(static_cast<unsigned char>(s[end]))) ++end;
    return s.substr(0, end);
}

bool looks_like_question(const std::string& text) {
    if (text.size() > kMaxQuestionBytes) return false;
    std::string s = text::normalize_name(text);
    if (s.empty()) return false;
    if (s.back() == '?') return true;
    std::string word = first_word(s);
    for (const char* w : kQuestionWords) {
        if (word == w) return true;
    }
    return false;
}

} // namespace chat_routing
