// Признаки вопроса к ИИ (openspec/changes/add-ai-chat, D1; спека ai-chat,
// «Признаки вопроса»). Чистая функция без вызова модели: решает, уходит ли
// текст в задание chat, а не в разбор траты. Покрыта tests/test_chat_routing.cpp.
#pragma once

#include <cstddef>
#include <string>

namespace chat_routing {

// Как input.text у wallet-ai: длиннее — не вопрос.
constexpr std::size_t kMaxQuestionBytes = 1000;

// Вопрос — текст до kMaxQuestionBytes, который после обрезки пробелов
// заканчивается на «?» или начинается с вопросительного слова из спеки
// («сколько», «покажи», «итого», …) без учёта регистра, «ё» = «е».
//
// Первое слово — до пробела или знака препинания, но дефис его не
// обрывает: «что-то 300» — трата, а не вопрос «что».
bool looks_like_question(const std::string& text);

// Первое слово в нормализованном виде (нижний регистр, «ё» → «е») — наружу
// ради тестов.
std::string first_word(const std::string& text);

} // namespace chat_routing
