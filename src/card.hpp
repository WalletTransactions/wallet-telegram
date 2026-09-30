// Сообщение черновика траты — текст и inline-клавиатура для текущего шага
// (org/docs/TODO_telegram_bot.md, «Подшаг 3 подробно»): вопрос о категории,
// вопрос о кошельке или карточка. Чистые функции, без сети — тесты через
// tests/test_bot.cpp.
#pragma once

#include <string>

#include "drafts.hpp"
#include "telegram_client.hpp"

namespace card {

// Сколько категорий на странице «Другая…» (по 2 в ряд).
constexpr std::size_t kPageSize = 8;

extern const char* const kConfirm;

struct Message {
    std::string text;
    telegram::Keyboard keyboard;
};

// today/yesterday — "YYYY-MM-DD" в поясе пользователя: дата траты
// показывается как «сегодня», «вчера», «24.09» или «15.03.2025».
// default_wallet_uuid — чтобы не предлагать «★ Сделать основным» тому, что
// уже основной.
Message render(const drafts::Draft& d, const std::string& default_wallet_uuid, const std::string& today,
               const std::string& yesterday);

// «350,00 ₽ · Продукты · Тинькофф · сегодня — молоко»; невыбранное —
// «категория?», «кошелёк?».
std::string summary(const drafts::Draft& d, const std::string& today, const std::string& yesterday);

// Страниц во «всех категориях», минимум 1.
int page_count(const drafts::Draft& d);

} // namespace card
