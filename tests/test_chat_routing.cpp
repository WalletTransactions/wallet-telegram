// Признаки вопроса (openspec/changes/add-ai-chat, D1, задача 1.1).
#include <string>

#include "chat_routing.hpp"
#include "test_framework.hpp"

using chat_routing::first_word;
using chat_routing::looks_like_question;

TEST(question_words_and_question_mark) {
    CHECK(looks_like_question("сколько за няню"));
    CHECK(looks_like_question("Сколько потратил в сентябре 2026")); // регистр, цифры не мешают
    CHECK(looks_like_question("итого?"));
    CHECK(looks_like_question("  СКОЛЬКО  за такси  ")); // пробелы по краям и внутри
    CHECK(looks_like_question("покажи траты за неделю"));
    CHECK(looks_like_question("баланс"));
    CHECK(looks_like_question("Чём я больше трачу")); // «ё» = «е»: «чём» → «чем»
    CHECK(looks_like_question("сколько, если без такси")); // запятая обрывает слово
    CHECK(looks_like_question("траты на такси за неделю?")); // «?» в конце — без слова из списка
    CHECK(looks_like_question("а такси?  "));
}

TEST(expenses_are_not_questions) {
    CHECK(!looks_like_question("няня 3000"));
    CHECK(!looks_like_question("что-то 300")); // дефис не обрывает слово: «что-то» ≠ «что»
    CHECK(!looks_like_question("потратил 300 на кофе"));
    CHECK(!looks_like_question("кофе 200"));
    CHECK(!looks_like_question("сколькотысяч 200")); // слово целиком, не префикс
    CHECK(!looks_like_question("привет"));
    CHECK(!looks_like_question(""));
    CHECK(!looks_like_question("   "));
}

TEST(question_length_limit) {
    std::string tail(chat_routing::kMaxQuestionBytes - std::string("сколько ").size(), 'x');
    CHECK(looks_like_question("сколько " + tail)); // ровно предел
    CHECK(!looks_like_question("сколько " + tail + "x"));
}

TEST(first_word_normalized) {
    CHECK_EQ(first_word("  Ёлка, привет"), std::string("елка"));
    CHECK_EQ(first_word("что-то 300"), std::string("что-то"));
    CHECK_EQ(first_word("итого?"), std::string("итого"));
    CHECK_EQ(first_word(""), std::string(""));
}
