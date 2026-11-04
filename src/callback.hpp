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

// Кнопки /ai (openspec/changes/add-ai-chat, D6) — префикс "ai:":
//
//   ai:sel:<provider>      выбрать провайдера
//   ai:consent:<provider>  «Согласен» на экране согласия
//   ai:revoke:<provider>   «Отозвать согласие»
//   ai:back                «Отмена» — вернуться на экран /ai
//
// provider — только [a-z]{1,16}; всё прочее с префиксом "ai:" — nullopt.
enum class AiKind { Select, Consent, Revoke, Back };

struct AiAction {
    AiKind kind = AiKind::Back;
    std::string provider; // пусто у Back
};

bool is_ai(const std::string& data); // начинается с "ai:"
std::string make_ai(AiKind kind, const std::string& provider = {});
std::optional<AiAction> parse_ai(const std::string& data);
const char* ai_kind_name(AiKind kind);

} // namespace callback
