// callback_data кнопок карточки траты (org/docs/TODO_telegram_bot.md,
// «Подшаг 3 подробно»): "<draft_id>:<действие>[:<число>]".
//
//   <id>:c:<i>   выбрана категория Draft::categories[i]
//   <id>:cp:<n>  все категории, страница n («Другая…», ‹ ›)
//   <id>:w:<i>   выбран кошелёк Draft::wallets[i] (в том числе «Всё равно в …»)
//   <id>:ce      «Категория…» на карточке
//   <id>:we      «Кошелёк…» на карточке
//   <id>:wd      «★ Сделать основным»
//   <id>:x       «✕» — черновик выброшен
//   <id>:ok      «✓ Записать» / «✓ Проверить ещё раз»
//
// callback_data приходит от клиента — недоверенные данные: parse строгий,
// а владельца черновика и границы индекса проверяет Bot.
#pragma once

#include <optional>
#include <string>

namespace callback {

enum class Kind { PickCategory, CategoryPage, PickWallet, EditCategory, EditWallet, MakeDefault, Cancel, Save };

struct Action {
    std::string draft_id; // только цифры, 1..20
    Kind kind = Kind::Cancel;
    int number = 0; // индекс или страница, 0..999; у действий без числа — 0
};

// Самое длинное — "18446744073709551615:cp:999", 27 байт: в лимит 64 байта
// Bot API влезает с запасом.
std::string make(const std::string& draft_id, Kind kind, int number = 0);
std::optional<Action> parse(const std::string& data);

// Для логов: "c", "cp", "w", ...
const char* kind_name(Kind kind);

} // namespace callback
