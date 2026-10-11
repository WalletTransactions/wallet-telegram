// Черновики трат (org/docs/TODO_telegram_bot.md, «Подшаг 3 подробно»): что
// бот понял из сообщения и что пользователь уточнил кнопками до записи.
//
// Хранятся в памяти: поток один, replicas: 1, потеря при рестарте допустима
// (§1.4) — кнопки старого сообщения ответят «устарело». Redis — вместе с
// пулом воркеров.
#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "expense/strict.hpp"
#include "expense/resolver.hpp"

namespace drafts {

constexpr std::time_t kTtlSecs = 15 * 60;
constexpr std::size_t kMaxPerUser = 20;

struct Draft {
    // update_id сообщения с тратой, десятичной строкой: уникален в пределах
    // бота, короткий (влезает в callback_data), а повторная доставка того же
    // апдейта даёт тот же id — в подшаге 5 и тот же batch_id.
    std::string id;
    std::int64_t tg_user_id = 0;
    std::string user_id; // пользователь кошелька: чат переподключили к другому — черновик устарел
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::time_t created_at = 0;

    parser::ParsedExpense expense;

    // Снимок справочника на момент сообщения. Индексы в callback_data — в эти
    // списки: не меняются между видами (кандидаты, все категории, карточка) и
    // не дают подсунуть чужой uuid. Переименование на телефоне кнопки не
    // ломает; удаление ловит 409 при записи (подшаг 4).
    std::vector<resolver::Category> categories;
    std::vector<resolver::Wallet> wallets;

    std::optional<std::size_t> category; // индекс в categories, выбранное
    std::optional<std::size_t> wallet;   // индекс в wallets, выбранное
    bool wallet_by_currency = false;     // выбран resolver'ом по валюте — назвать в карточке
    bool offer_remember = false;         // кошелька по умолчанию нет — предложить «★ Сделать основным»

    // Done — записано: черновик живёт до TTL, чтобы повторный клик «✓»
    // ответил «Уже записано», а не «устарел» поверх «✓ Записано».
    enum class Step { Category, Wallet, Card, Done };
    Step step = Step::Card;
    // Запись пробовали, исход неизвестен (таймаут, 5xx): push мог
    // примениться. Правка закрыта — повтор только тем же батчем («✓
    // Проверить ещё раз»), иначе сохранённый ответ сказал бы «записано» про
    // старое содержимое.
    bool attempted = false;

    // Вопрос о категории: почему спрашиваем и кого предложить.
    resolver::CategoryPick::Why category_why = resolver::CategoryPick::Why::None;
    std::vector<std::size_t> category_candidates;
    // -1 — показаны кандидаты, иначе страница всех категорий.
    int category_page = -1;

    // Вопрос о кошельке.
    resolver::WalletPick::Why wallet_why = resolver::WalletPick::Why::None;
    std::vector<std::size_t> wallet_candidates;
    std::optional<std::size_t> wallet_force; // «Всё равно в <кошелёк>»
    // Выбор с карточки («Кошелёк…»): кошелёк другой валюты, чем в
    // сообщении, — отказ (конвертации нет). Кандидатов resolver'а это не
    // касается — их он уже подобрал под валюту.
    bool wallet_strict_currency = false;
};

class Drafts {
public:
    // Кладёт черновик (тот же id — заменяет). Попутно выбрасывает истёкшие и,
    // если у пользователя их больше kMaxPerUser, самый старый.
    Draft& put(Draft draft, std::time_t now);
    // nullptr — нет или истёк (истёкший удаляется).
    Draft* find(const std::string& id, std::time_t now);
    void erase(const std::string& id);
    std::size_t size() const { return by_id_.size(); }

private:
    std::unordered_map<std::string, Draft> by_id_;
};

} // namespace drafts
