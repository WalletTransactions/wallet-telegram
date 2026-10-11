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

#include "ai_client.hpp"
#include "auth_client.hpp"
#include "drafts.hpp"
#include "link_repo.hpp"
#include "nlohmann/json.hpp"
#include "clients/reports_client.hpp"
#include "expense/resolver.hpp"
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

    // Разбор свободного текста через wallet-ai (org/docs/TODO_wallet_ai.md,
    // шаг 4). nullptr (по умолчанию) — ИИ выключен, бот ведёт себя как без
    // него. Указатель живёт дольше бота.
    void enable_ai(ai::AiApi* ai) { ai_ = ai; }

    // Есть задания wallet-ai, чьего итога ждут сообщения «Разбираю…», —
    // цикл поллинга тогда ждёт getUpdates недолго и чаще зовёт ai_tick.
    bool has_pending_ai() const { return !pending_ai_.empty(); }

    // Опрос заданий: готовые — правка «Разбираю…» в карточку или запасной
    // ответ, зависшие дольше kAiGiveUpSecs — отмена и запасной ответ. Тот же
    // поток, что handle(): HTTP-клиент Telegram, база и черновики однопоточны.
    void ai_tick();

private:
    void handle_start(const telegram::Update& update, const std::string& code);
    void handle_wallets(const telegram::Update& update, const links::Link& link);
    void handle_categories(const telegram::Update& update, const links::Link& link);
    void handle_last(const telegram::Update& update, const links::Link& link, const std::string& args);
    void handle_default(const telegram::Update& update, const links::Link& link, const std::string& args);
    void handle_tz(const telegram::Update& update, const links::Link& link, const std::string& args);
    void handle_unlink(const telegram::Update& update, const links::Link& link);
    void handle_expense(const telegram::Update& update, const links::Link& link);

    // Что показать, если ИИ не помог (недоступен, не нашёл трату, ответ не
    // прошёл проверки, не успел): ровно то, что бот ответил бы без ИИ.
    struct AiFallback {
        enum class Kind {
            NotUnderstood, // текст не с суммы: «Не понял. Пример…»
            ParseError,    // строгий разбор упал на сумме: прежний текст ошибки
            StrictDraft,   // строгий разбор прошёл, категории нет: прежние кнопки
        };
        Kind kind = Kind::NotUnderstood;
        parser::ParseError::Reason reason = parser::ParseError::Reason::NoAmount;
        parser::ParsedExpense expense;
    };
    struct PendingAi {
        std::string job_id;
        telegram::Update update; // сообщение с тратой — id черновика, чат, отправитель
        std::int64_t message_id = 0; // «Разбираю…» — его и правим
        std::time_t created_at = 0;
        AiFallback fallback;
    };
    // Создаёт задание parse_expense. Создано — «Разбираю…» и запись в
    // pending_ai_; нет — сразу запасной ответ.
    void try_ai(const telegram::Update& update, const links::Link& link, const AiFallback& fallback);
    // true — с заданием покончено (итог показан), его можно забыть.
    bool process_pending_ai(PendingAi& p);
    // edit_message_id != 0 — править это сообщение, иначе отправить новое.
    void run_fallback(const telegram::Update& update, const links::Link& link, const AiFallback& fallback,
                      std::int64_t edit_message_id);
    // Общий хвост строгого формата и ИИ: resolver -> черновик -> карточка.
    // from_text — черновик из ИИ (пометка на карточке).
    void start_draft(const telegram::Update& update, const links::Link& link, const parser::ParsedExpense& e,
                     const resolver::Accounts& acc, bool from_text, std::int64_t edit_message_id);
    // Текст без клавиатуры: правка edit_message_id или новое сообщение.
    void respond(const telegram::Update& update, const std::string& text, std::int64_t edit_message_id);
    // Включён ли ИИ (GET /api/ai/status токеном пользователя), кеш на минуту
    // — для строки в /help. Не удалось узнать — false.
    bool ai_enabled_for(const links::Link& link);
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

    ai::AiApi* ai_ = nullptr;
    std::vector<PendingAi> pending_ai_;
    std::optional<bool> ai_enabled_;
    std::time_t ai_enabled_checked_at_ = 0;
};

// ИИ зовётся только для коротких сообщений с цифрой: без цифры трату не
// записать, длинный текст — не трата и лишняя нагрузка на GPU.
constexpr std::size_t kAiMaxTextBytes = 300;
// Сколько ждать итога задания, прежде чем показать запасной ответ (дедлайн
// генерации в wallet-ai — 60 с, плюс очередь).
constexpr std::time_t kAiGiveUpSecs = 90;

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
extern const char* const kAiParsing;
extern const char* const kAiNotUnderstood;
extern const char* const kHelpAi;
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
