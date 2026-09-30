// Логика бота (org/docs/TODO_telegram_bot.md, фаза 1): что ответить на
// апдейт. Не знает ни про HTTP, ни про Postgres — всё через интерфейсы
// TelegramApi/AuthApi/ReportsApi/LinkStore, поэтому целиком покрыта
// юнит-тестами на фейках (tests/test_bot.cpp).
//
// Шаги 4–5 плана: связывание (/start <код>), /help и команды чтения —
// /wallets, /categories, /last, /default, /tz, /unlink. Шаг 6, подшаги 2–3:
// трата разбирается и разрешается в счета, дальше — черновик: вопросы
// кнопками, карточка, «✕». Записи пока нет.
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "auth_client.hpp"
#include "drafts.hpp"
#include "link_repo.hpp"
#include "nlohmann/json.hpp"
#include "reports_client.hpp"
#include "resolver.hpp"
#include "sessions.hpp"
#include "telegram_client.hpp"
#include "wallet_client.hpp"

namespace bot {

struct Command {
    std::string name; // без '/' и без "@botname", в нижнем регистре; пусто — не команда
    std::string args; // остаток строки без ведущих/хвостовых пробелов
};

// "/start@WalletTransaction_bot abc" -> {"start", "abc"}. Упоминание
// другого бота ("/start@other_bot") — не наша команда, name пустой.
// bot_username — без '@', сравнение без учёта регистра; пустой — любое
// упоминание принимается.
Command parse_command(const std::string& text, const std::string& bot_username);

// Код связывания от google-auth — base64url, 16..64 символа (сейчас 32).
// Проверяется до похода в google-auth, чтобы мусор из /start не уходил во
// внутренний эндпоинт.
bool looks_like_link_code(const std::string& s);

struct Config {
    std::string bot_username;
    std::string default_tz = "UTC";
    // Пусто — бот открыт всем. Непусто — отвечает только этим Telegram id
    // (TELEGRAM_ALLOWED_USER_IDS, закрытое тестирование).
    std::unordered_set<std::int64_t> allowed_user_ids;
    // Каталог tzdata для проверки /tz — подменяется в тестах.
    std::string zoneinfo_dir = "/usr/share/zoneinfo";
    // Источник «сейчас» — подменяется в тестах; пусто — std::time(nullptr).
    std::function<std::time_t()> clock;
};

class Bot {
public:
    Bot(telegram::TelegramApi& tg, auth::AuthApi& auth, reports::ReportsApi& reports, wallet::WalletApi& wallet,
        links::LinkStore& store, Config config);

    // Обрабатывает один апдейт. Исключения TelegramApi/LinkStore пробрасывает
    // наружу — решение «что делать с апдейтом, который не удалось
    // обработать», принимает цикл поллинга (run_poll_loop).
    void handle(const telegram::Update& update);

private:
    void handle_start(const telegram::Update& update, const std::string& code);
    void handle_wallets(const telegram::Update& update, const links::Link& link);
    void handle_categories(const telegram::Update& update, const links::Link& link);
    void handle_last(const telegram::Update& update, const links::Link& link, const std::string& args);
    void handle_default(const telegram::Update& update, const links::Link& link, const std::string& args);
    void handle_tz(const telegram::Update& update, const links::Link& link, const std::string& args);
    void handle_unlink(const telegram::Update& update, const links::Link& link);
    void handle_expense(const telegram::Update& update, const links::Link& link);
    // Нажатие кнопки черновика. Проверки те же, что у сообщения (allowlist,
    // личный чат, связка), плюс владелец черновика; ответ — ровно один.
    void handle_callback(const telegram::Update& update);

    // «✓ Записать»: сверка со свежим справочником, push, правка сообщения по
    // исходу. Может удалить черновик (читатель, ошибка протокола) — d после
    // вызова не трогать.
    struct SaveReply {
        std::string text; // ответ на нажатие
        bool alert = false;
        std::string result; // для лога
    };
    SaveReply save_draft(const telegram::Update& update, const links::Link& link, drafts::Draft& d,
                         std::int64_t message_id, const std::string& today, const std::string& yesterday);

    // Вызов сервиса токеном пользователя: call(token) делает запрос и
    // возвращает true, если сервис ответил 401 — тогда ровно один повтор с
    // новым токеном (сменились ключи google-auth и т.п.). false от
    // with_token — пользователю уже ответили (связку удалили или google-auth
    // недоступен), результата нет; true — call отработал, результат у
    // вызывающего (в том числе повторный 401).
    bool with_token(const telegram::Update& update, const links::Link& link,
                    const std::function<bool(const std::string&)>& call);
    // GET к wallet-report от имени пользователя. nullopt — ответ
    // пользователю уже отправлен (сессия отозвана или сервис недоступен),
    // команде остаётся только выйти.
    std::optional<nlohmann::json> report_get(const telegram::Update& update, const links::Link& link,
                                             const std::string& path_and_query);
    // Кошельки и категории пользователя (GET /api/reports/accounts) из кеша
    // на kAccountsTtlSecs; fresh — мимо кеша, с обновлением записи. nullptr —
    // ответ пользователю уже отправлен, как у report_get. Указатель живёт до
    // следующего вызова accounts/forget_accounts.
    const resolver::Accounts* accounts(const telegram::Update& update, const links::Link& link, bool fresh);
    void forget_accounts(std::int64_t tg_user_id);
    // Валюта эквивалента для отчётов (они требуют eq_currency): валюта
    // кошелька по умолчанию, иначе первого кошелька.
    static std::string eq_currency_for(const links::Link& link, const std::vector<resolver::Wallet>& wallets);

    void reply(const telegram::Update& update, const std::string& text);
    std::time_t now() const;

    telegram::TelegramApi& tg_;
    auth::AuthApi& auth_;
    reports::ReportsApi& reports_;
    wallet::WalletApi& wallet_;
    links::LinkStore& store_;
    Config config_;
    sessions::Sessions sessions_;

    // Кеш справочника счетов по tg_user_id (§3.3): поток один — без мьютекса.
    // Сбрасывается /wallets и /categories (они читают fresh), /start, /unlink.
    struct CachedAccounts {
        resolver::Accounts accounts;
        std::time_t fetched_at = 0;
    };
    std::unordered_map<std::int64_t, CachedAccounts> accounts_cache_;

    drafts::Drafts drafts_;
};

// Сколько живёт запись кеша справочника счетов.
constexpr std::time_t kAccountsTtlSecs = 60;

// Тексты ответов — наружу ради тестов (сравнение без копирования строк).
namespace texts {
extern const char* const kHelp;
extern const char* const kStartWithoutCode;
extern const char* const kLinked;
extern const char* const kCodeRejected;
extern const char* const kBadCode;
extern const char* const kUnavailable;
extern const char* const kNotLinked;
extern const char* const kRevoked;
extern const char* const kClosedBeta;
extern const char* const kUnknown;
extern const char* const kUnlinked;
extern const char* const kNoWallets;
extern const char* const kDraftStale;
} // namespace texts

// Цикл long polling. Апдейты обрабатываются строго по порядку, в этом же
// потоке: у одного экземпляра бота (replicas: 1) это и есть гарантия
// «апдейты одного пользователя — по порядку» из §1.5 без отдельного пула.
//
// Offset сохраняется в базе после каждого апдейта: после рестарта старые
// апдейты не переобрабатываются. Апдейт, на котором Bot::handle бросил
// исключение, логируется и пропускается — иначе один «ядовитый» апдейт
// навсегда остановил бы бота. Ошибки самого getUpdates — повтор с
// экспоненциальной паузой до 30 с.
//
// stop проверяется между вызовами getUpdates; выход — в пределах
// poll_timeout_secs.
void run_poll_loop(telegram::TelegramApi& tg, links::LinkStore& store, Bot& bot,
                   const std::atomic<bool>& stop, int poll_timeout_secs);

} // namespace bot
