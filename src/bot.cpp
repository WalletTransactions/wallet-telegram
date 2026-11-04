#include "bot.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "callback.hpp"
#include "card.hpp"
#include "chat_routing.hpp"
#include "expense/strict.hpp"
#include "expense/text.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"
#include "util/tz.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace bot {

namespace texts {
const char* const kHelp =
    "Я записываю траты в ваш кошелёк WalletTransactions и показываю его состояние.\n"
    "\n"
    "Трата — сообщением: <сумма> [валюта] <категория> [@кошелёк] [дата] [// комментарий]\n"
    "• 350 продукты\n"
    "• 1200,50 кафе @тинькофф вчера // обед\n"
    "• 12 € такси 24.09\n"
    "Что неясно — спрошу кнопками, запишу после «✓ Записать».\n"
    "\n"
    "/wallets — кошельки и балансы\n"
    "/categories — категории\n"
    "/last [N] — последние операции (по умолчанию 10)\n"
    "/default <кошелёк> — кошелёк по умолчанию для трат\n"
    "/tz <пояс> — часовой пояс, например /tz Europe/Moscow\n"
    "/unlink — отключить бота от кошелька\n"
    "/help — эта справка";
const char* const kStartWithoutCode =
    "Чтобы подключить бота, откройте веб-кабинет WalletTransactions → Доступ и приглашения → Telegram-бот и "
    "нажмите «Подключить». Ссылка оттуда откроет этот чат уже с кодом.";
const char* const kLinked = "Готово, бот подключён к вашему кошельку.\n/help — что я умею.";
const char* const kCodeRejected =
    "Код подключения устарел или уже использован. Получите новый в веб-кабинете: Доступ и приглашения → Telegram-бот.";
const char* const kBadCode =
    "Не похоже на код подключения. Откройте ссылку из веб-кабинета: Доступ и приглашения → Telegram-бот.";
const char* const kUnavailable = "Сервис временно недоступен, попробуйте чуть позже.";
const char* const kNotLinked =
    "Бот ещё не подключён к кошельку. Откройте веб-кабинет → Доступ и приглашения → Telegram-бот → «Подключить».";
const char* const kRevoked =
    "Бот отключён от кошелька (сессию отозвали в веб-кабинете или она истекла). "
    "Подключите заново: веб-кабинет → Доступ и приглашения → Telegram-бот.";
const char* const kClosedBeta = "Бот пока в закрытом тестировании.";
const char* const kUnknown = "Пока не понимаю это сообщение. /help — что я умею.";
const char* const kUnlinked =
    "Бот отключён от кошелька. Подключить снова: веб-кабинет → Доступ и приглашения → Telegram-бот.";
const char* const kNoWallets = "Кошельков пока нет — заведите их в приложении.";
const char* const kDraftStale = "Черновик устарел — отправьте трату ещё раз.";
const char* const kAiParsing = "Разбираю…";
const char* const kAiNotUnderstood = "Не понял. Пример: 350 продукты @тинькофф вчера";
const char* const kHelpAi = "Можно и своими словами: «кофе 200», «вчера такси 450 с тинькоффа».\n";
const char* const kHelpAsk = "Можно спросить: «сколько за няню в сентябре?», «чем октябрь отличается от сентября?»\n";
const char* const kHelpAiCommands = "/ai — кто отвечает на вопросы\n/new — новый разговор\n";
const char* const kChatThinking = "Думаю…";
const char* const kChatTimeout = "ИИ не успел ответить, попробуйте позже.";
const char* const kChatProviderUnavailable = "Выбранный ИИ сейчас недоступен. /ai — выбрать другой.";
const char* const kChatOff = "ИИ сейчас выключен.";
const char* const kChatBusy = "ИИ занят, попробуйте через минуту.";
const char* const kChatNoData = "Не удалось получить данные кошелька, попробуйте позже.";
const char* const kChatRefused = "ИИ не стал отвечать на этот вопрос.";
const char* const kChatFailed = "ИИ не смог ответить, попробуйте позже.";
const char* const kChatNew = "Начали новый разговор.";
const char* const kAiNow = "Сейчас отвечает: ";
// Константа бота, а не ответ wallet-ai (D6): текст согласия должен быть виден,
// даже если wallet-ai вернул неожиданный title.
const char* const kAiConsent =
    "Вопросы и данные кошелька, нужные для ответа (суммы, категории, комментарии операций), будут отправляться в "
    "Anthropic. Согласны?";
const char* const kAiDone = "Готово";
const char* const kAiProviderNotAvailable = "Сейчас недоступен";
} // namespace texts

namespace {

// Короткое имя провайдера для экрана и кнопок; неизвестный — title из
// wallet-ai, иначе id.
std::string provider_name(const std::string& id, const json& entry) {
    if (id == "lmstudio") return "LM Studio";
    if (id == "claude") return "Claude";
    if (entry.is_object() && entry.contains("title") && entry["title"].is_string()) return entry["title"].get<std::string>();
    return id;
}

} // namespace

AiScreen render_ai_screen(const json& settings) {
    AiScreen s;
    std::string selected = settings.value("provider", std::string{});
    json selected_entry;
    std::vector<telegram::Button> providers;
    std::vector<std::vector<telegram::Button>> revokes;
    for (const auto& p : settings.value("providers", json::array())) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string()) continue;
        std::string id = p["id"].get<std::string>();
        if (id == selected) selected_entry = p;
        // Кнопку получает только то, что прошло бы строгий разбор callback.
        if (!callback::parse_ai(callback::make_ai(callback::AiKind::Select, id))) continue;
        if (p.value("available", false)) {
            providers.push_back({(id == selected ? "✓ " : "") + provider_name(id, p),
                                 callback::make_ai(callback::AiKind::Select, id)});
        }
        if (p.value("cloud", false) && p.value("consented", false)) {
            revokes.push_back({{"Отозвать согласие", callback::make_ai(callback::AiKind::Revoke, id)}});
        }
    }
    s.text = std::string(texts::kAiNow) + provider_name(selected, selected_entry);
    if (!providers.empty()) s.keyboard.push_back(providers);
    for (auto& row : revokes) s.keyboard.push_back(std::move(row));
    return s;
}

AiScreen render_ai_consent(const std::string& provider) {
    AiScreen s;
    s.text = texts::kAiConsent;
    s.keyboard = {{{"Согласен", callback::make_ai(callback::AiKind::Consent, provider)},
                   {"Отмена", callback::make_ai(callback::AiKind::Back)}}};
    return s;
}

namespace {

// Telegram режет сообщения длиннее 4096 символов; байты UTF-8 ≥ символов,
// поэтому с запасом.
constexpr std::size_t kMaxMessageBytes = 4000;
constexpr int kLastDefault = 10;
constexpr int kLastMax = 30;

std::string trim(const std::string& s) {
    auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if(s.begin(), s.end(), not_space);
    auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return begin < end ? std::string(begin, end) : std::string{};
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string str(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

void count_update(const std::string& type) {
    metrics::inc_counter("tg_updates_total", "Апдейты Telegram по типу обработки.", "type", type);
}

// Что стало с сообщением-тратой (§3.4): resolved — сразу карточка,
// ambiguous — вопрос кнопками, rejected — отказ (нет кошелька в валюте,
// лишние знаки), parse_failed — не разобрано (причина — tg_parse_fail_total).
void count_expense(const char* result) {
    metrics::inc_counter("tg_expense_total", "Сообщения-траты по итогу разбора.", "result", result);
}

// Исход «✓ Записать» — SaveReply::result (saved, unavailable, lost, ...).
void count_save(const std::string& result) {
    metrics::inc_counter("tg_expense_save_total", "Нажатия «✓ Записать» по исходу.", "result", result);
}

// Похоже ли сообщение на трату: начинается с цифры или знака валюты ($12,
// €5, ₽350). Дешёвая проверка до похода в wallet-report — обычная болтовня
// («привет») не должна тянуть справочник счетов.
bool looks_like_expense(const std::string& text) {
    std::string s = trim(text);
    if (s.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(s[0])) != 0 || s[0] == '$') return true;
    return s.compare(0, 3, "\xE2\x82\xAC") == 0    // €
           || s.compare(0, 3, "\xE2\x82\xBD") == 0; // ₽
}

// Звать ли ИИ: есть цифра (без суммы трату не записать — «кофе двести» не
// разбираем) и сообщение короткое.
bool ai_eligible(const std::string& text) {
    if (text.size() > kAiMaxTextBytes) return false;
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// result.expense задания parse_expense — тот же ParsedExpense, что у строгого
// формата (wallet-common/expense), уже проверенный wallet-ai.
parser::ParsedExpense expense_from_json(const json& e) {
    parser::ParsedExpense out;
    auto s = [&e](const char* k) { return e.contains(k) && e[k].is_string() ? e[k].get<std::string>() : std::string{}; };
    out.amount = s("amount");
    out.fraction_digits = e.contains("fraction_digits") && e["fraction_digits"].is_number_integer()
                              ? e["fraction_digits"].get<int>()
                              : 0;
    out.currency_hint = s("currency_hint");
    out.category = s("category");
    out.wallet = s("wallet");
    out.datetime = s("datetime");
    out.comment = s("comment");
    return out;
}

void count_ai_job(const char* result) {
    metrics::inc_counter("tg_ai_job_total", "Создание заданий wallet-ai по исходу.", "result", result);
}

const char* parse_error_text(parser::ParseError::Reason reason) {
    using R = parser::ParseError::Reason;
    switch (reason) {
    case R::NoAmount:
        return texts::kUnknown;
    case R::BadAmount:
        return "Не понял сумму. Пример: 1200,50 кафе @тинькофф вчера // обед";
    case R::ZeroAmount:
        return "Сумма должна быть больше нуля.";
    case R::BadDate:
        return "Нет такой даты. Можно: сегодня, вчера, позавчера, 24.09 или 24.09.2025.";
    case R::FutureDate:
        return "Эта дата ещё не наступила — трату можно записать сегодняшним или прошедшим днём.";
    }
    return texts::kUnknown;
}

const char* category_why_name(const resolver::CategoryPick& p) {
    using W = resolver::CategoryPick::Why;
    if (p.kind == resolver::CategoryPick::Kind::Found) return "found";
    switch (p.why) {
    case W::Missing: return "missing";
    case W::NotFound: return "not_found";
    case W::Ambiguous: return "ambiguous";
    case W::None: break;
    }
    return "ask";
}

const char* wallet_why_name(const resolver::WalletPick& p) {
    using W = resolver::WalletPick::Why;
    if (p.kind == resolver::WalletPick::Kind::Found) return p.by_currency ? "found_by_currency" : "found";
    switch (p.why) {
    case W::NotGiven: return "not_given";
    case W::NotFound: return "not_found";
    case W::Ambiguous: return "ambiguous";
    case W::CurrencyMismatch: return "currency_mismatch";
    case W::NoWalletInCurrency: return "no_wallet_in_currency";
    case W::NoWallets: return "no_wallets";
    case W::None: break;
    }
    return "ask";
}

// «в JPY только целые суммы» — отказ по precision кошелька.
std::string precision_text(const resolver::Wallet& w) {
    std::string limit = w.precision == 0   ? std::string("только целые суммы")
                        : w.precision == 1 ? std::string("не больше 1 знака после запятой")
                                           : "не больше " + std::to_string(w.precision) + " знаков после запятой";
    return "В кошельке «" + w.name + "» (" + w.iso_code + ") — " + limit + ".";
}

template <typename T>
std::size_t index_of(const std::vector<T>& items, const std::string& uuid) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].uuid == uuid) return i;
    }
    return 0; // не бывает: resolver берёт кандидатов из того же справочника
}

template <typename T>
std::vector<std::size_t> indices_of(const std::vector<T>& all, const std::vector<T>& picked) {
    std::vector<std::size_t> out;
    for (const auto& p : picked) out.push_back(index_of(all, p.uuid));
    return out;
}

// Пространство имён UUIDv5 для batch_id и uuid операций бота. Постоянное:
// сменить — значит, повтор записи после выката получил бы новый батч и
// операцию-дубль.
constexpr const char* kDraftNamespace = "c7bc3451-3600-4574-a286-0e46ead486de";

// Снимок справочника черновика — свежим (перед записью, §3.3): имена могли
// смениться, счёт — удалиться на телефоне. Удаление там мягкое, и внешний
// ключ операции удалённую строку всё равно находит — без этой сверки трата
// молча ушла бы на удалённую категорию. Выбранное ищется по uuid; true —
// что-то из выбранного пропало, вопросы переключены на все варианты.
bool refresh_snapshot(drafts::Draft& d, const resolver::Accounts& acc) {
    std::string wallet_uuid = d.wallet ? d.wallets[*d.wallet].uuid : std::string{};
    std::string category_uuid = d.category ? d.categories[*d.category].uuid : std::string{};
    d.wallets = acc.wallets;
    d.categories = acc.categories;
    auto find = [](const auto& items, const std::string& uuid) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].uuid == uuid) return i;
        }
        return std::nullopt;
    };
    d.wallet = wallet_uuid.empty() ? std::nullopt : find(d.wallets, wallet_uuid);
    d.category = category_uuid.empty() ? std::nullopt : find(d.categories, category_uuid);
    bool lost = (!wallet_uuid.empty() && !d.wallet) || (!category_uuid.empty() && !d.category);

    // Прежние кандидаты — индексы в старом снимке; спрашиваем заново обо всём.
    d.category_why = resolver::CategoryPick::Why::None;
    d.category_candidates.clear();
    d.category_page = 0;
    d.wallet_why = resolver::WalletPick::Why::None;
    d.wallet_candidates.clear();
    for (std::size_t i = 0; i < d.wallets.size(); ++i) d.wallet_candidates.push_back(i);
    d.wallet_force.reset();
    d.wallet_strict_currency = true;
    d.wallet_by_currency = false;
    return lost;
}

void next_step(drafts::Draft& d) {
    if (!d.category) d.step = drafts::Draft::Step::Category;
    else if (!d.wallet) d.step = drafts::Draft::Step::Wallet;
    else d.step = drafts::Draft::Step::Card;
}

// Черновик из решения resolver'а: найденное — заполнено, остальное —
// вопросы с кандидатами (индексами в снимке справочника).
drafts::Draft make_draft(const parser::ParsedExpense& e, const resolver::Resolution& r, const resolver::Accounts& acc,
                         const std::string& default_wallet_uuid) {
    drafts::Draft d;
    d.expense = e;
    d.categories = acc.categories;
    d.wallets = acc.wallets;

    if (r.category.kind == resolver::CategoryPick::Kind::Found) {
        d.category = index_of(d.categories, r.category.found.uuid);
    } else {
        d.category_why = r.category.why;
        d.category_candidates = indices_of(d.categories, r.category.candidates);
        // Предложить некого — сразу все категории.
        if (d.category_candidates.empty()) d.category_page = 0;
    }

    if (r.wallet.kind == resolver::WalletPick::Kind::Found) {
        d.wallet = index_of(d.wallets, r.wallet.found.uuid);
        d.wallet_by_currency = r.wallet.by_currency;
    } else {
        d.wallet_why = r.wallet.why;
        d.wallet_candidates = indices_of(d.wallets, r.wallet.candidates);
        if (r.wallet.force) d.wallet_force = index_of(d.wallets, r.wallet.force->uuid);
    }

    // Кошелька по умолчанию нет (или он удалён), а выбирать есть из чего —
    // на карточке «★ Сделать кошелёк основным».
    bool has_default = std::any_of(d.wallets.begin(), d.wallets.end(),
                                   [&](const resolver::Wallet& w) { return w.uuid == default_wallet_uuid; });
    d.offer_remember = !has_default && d.wallets.size() > 1;
    next_step(d);
    return d;
}

} // namespace

Command parse_command(const std::string& text, const std::string& bot_username) {
    Command cmd;
    std::string s = trim(text);
    if (s.size() < 2 || s[0] != '/') return cmd;

    std::size_t space = s.find_first_of(" \t\n\r");
    std::string head = s.substr(1, space == std::string::npos ? std::string::npos : space - 1);
    std::string args = space == std::string::npos ? std::string{} : trim(s.substr(space));

    std::size_t at = head.find('@');
    if (at != std::string::npos) {
        std::string mention = head.substr(at + 1);
        head = head.substr(0, at);
        if (!bot_username.empty() && ascii_lower(mention) != ascii_lower(bot_username)) return cmd;
    }
    if (head.empty()) return cmd;
    cmd.name = ascii_lower(head);
    cmd.args = args;
    return cmd;
}

std::vector<telegram::BotCommand> menu_commands(bool ai_enabled) {
    std::vector<telegram::BotCommand> out = {
        {"wallets", "Кошельки и балансы"},
        {"last", "Последние операции"},
        {"categories", "Категории"},
    };
    if (ai_enabled) {
        out.push_back({"ai", "Кто отвечает на вопросы"});
        out.push_back({"new", "Новый разговор с ИИ"});
    }
    out.push_back({"default", "Кошелёк по умолчанию для трат"});
    out.push_back({"tz", "Часовой пояс"});
    out.push_back({"help", "Что умеет бот"});
    out.push_back({"unlink", "Отключить бота от кошелька"});
    return out;
}

bool looks_like_link_code(const std::string& s) {
    if (s.size() < 16 || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    });
}

Bot::Bot(telegram::TelegramApi& tg, auth::AuthApi& auth, reports::ReportsApi& reports, wallet::WalletApi& wallet,
         links::LinkStore& store, Config config)
    : tg_(tg),
      auth_(auth),
      reports_(reports),
      wallet_(wallet),
      store_(store),
      config_(std::move(config)),
      sessions_(auth, store) {}

std::time_t Bot::now() const { return config_.clock ? config_.clock() : std::time(nullptr); }

void Bot::reply(const telegram::Update& update, const std::string& text) {
    tg_.send_message(update.chat_id, text::truncate_utf8(text, kMaxMessageBytes), {});
}

void Bot::handle(const telegram::Update& update) {
    if (update.has_callback) {
        count_update("callback");
        handle_callback(update);
        return;
    }
    if (!update.has_message) {
        count_update("other");
        return;
    }
    // Только личные чаты (§2, п. 11): в группе траты и балансы одного
    // человека увидели бы все участники. Молча — без ответа в группу.
    if (update.chat_type != "private") {
        count_update("ignored_non_private");
        return;
    }
    if (!config_.allowed_user_ids.empty() && config_.allowed_user_ids.count(update.from_id) == 0) {
        count_update("rejected_not_allowed");
        reply(update, texts::kClosedBeta);
        return;
    }

    Command cmd = parse_command(update.text, config_.bot_username);
    if (cmd.name == "start") {
        count_update("start");
        if (cmd.args.empty()) {
            reply(update, texts::kStartWithoutCode);
        } else {
            handle_start(update, cmd.args);
        }
        return;
    }
    if (cmd.name == "help") {
        count_update("help");
        std::string help = texts::kHelp;
        if (ai_ != nullptr) {
            auto link = store_.find_link(update.from_id);
            // Строки про свободный текст, вопросы и команды ИИ — только когда
            // ИИ правда работает.
            static const std::string kAfter = "запишу после «✓ Записать».\n";
            static const std::string kBeforeHelp = "/help — эта справка";
            if (link && ai_enabled_for(*link)) {
                std::size_t cmds = help.find(kBeforeHelp);
                if (cmds != std::string::npos) help.insert(cmds, texts::kHelpAiCommands);
                std::size_t at = help.find(kAfter);
                if (at != std::string::npos) {
                    help.insert(at + kAfter.size(), std::string(texts::kHelpAi) + texts::kHelpAsk);
                }
            }
        }
        reply(update, help);
        return;
    }

    static const std::unordered_set<std::string> kLinkedCommands = {"wallets", "categories", "last",
                                                                    "default", "tz",         "unlink"};
    // Команды ИИ — только при заданном WALLET_AI_URL; без него — непонятое.
    static const std::unordered_set<std::string> kAiCommands = {"new", "ai"};
    bool ai_command = ai_ != nullptr && kAiCommands.count(cmd.name) != 0;
    if (kLinkedCommands.count(cmd.name) != 0 || ai_command) {
        count_update(cmd.name);
        auto link = store_.find_link(update.from_id);
        if (!link) {
            reply(update, texts::kNotLinked);
            return;
        }
        applog::set_user(link->user_id);
        if (cmd.name == "wallets") handle_wallets(update, *link);
        else if (cmd.name == "categories") handle_categories(update, *link);
        else if (cmd.name == "last") handle_last(update, *link, cmd.args);
        else if (cmd.name == "default") handle_default(update, *link, cmd.args);
        else if (cmd.name == "tz") handle_tz(update, *link, cmd.args);
        else if (cmd.name == "new") handle_new(update, *link);
        else if (cmd.name == "ai") handle_ai(update, *link);
        else handle_unlink(update, *link);
        return;
    }

    if (cmd.name.empty() && looks_like_expense(update.text)) {
        count_update("expense");
        auto link = store_.find_link(update.from_id);
        if (!link) {
            reply(update, texts::kNotLinked);
            return;
        }
        applog::set_user(link->user_id);
        handle_expense(update, *link);
        return;
    }

    // Вопрос к ИИ (D2): ответ на уточнение — «сентябрь» после «Укажите
    // период.» не похож ни на вопрос, ни на трату, — или признаки вопроса.
    // Текст с цифрой в окне уточнения идёт в разбор траты («кофе 200» после
    // вопроса к ИИ — всё ещё трата), а not_expense оттуда — в чат (D3).
    // Без связки — общий ответ ниже («подключите»), wallet-ai не зовётся.
    bool reply_to_chat = awaiting_reply(update.from_id, now()) && !ai_eligible(update.text);
    if (cmd.name.empty() && ai_ != nullptr && (reply_to_chat || chat_routing::looks_like_question(update.text))) {
        auto link = store_.find_link(update.from_id);
        if (link) {
            count_update("question");
            applog::set_user(link->user_id);
            start_chat(update, *link, /*edit_message_id=*/0, /*fallback=*/nullptr);
            return;
        }
    }

    // Не с суммы, но цифра есть («кофе 200») — свободным текстом через ИИ.
    if (cmd.name.empty() && ai_ != nullptr && ai_eligible(update.text)) {
        count_update("expense_text");
        auto link = store_.find_link(update.from_id);
        if (!link) {
            reply(update, texts::kNotLinked);
            return;
        }
        applog::set_user(link->user_id);
        AiFallback f;
        f.kind = AiFallback::Kind::NotUnderstood;
        try_ai(update, *link, f);
        return;
    }

    count_update("unknown");
    reply(update, store_.find_link(update.from_id) ? texts::kUnknown : texts::kNotLinked);
}

void Bot::handle_start(const telegram::Update& update, const std::string& code) {
    if (!looks_like_link_code(code)) {
        reply(update, texts::kBadCode);
        return;
    }

    auth::ExchangeResult result = auth_.exchange(code, update.from_id, update.from_username);
    switch (result.status) {
    case auth::ExchangeResult::Status::Ok:
        break;
    case auth::ExchangeResult::Status::CodeRejected:
        applog::info("telegram link code rejected", {{"event", "tg_link_rejected"}, {"tg_user_id", update.from_id}});
        reply(update, texts::kCodeRejected);
        return;
    case auth::ExchangeResult::Status::BadRequest:
        applog::warn("telegram link exchange: bad request",
                     {{"event", "tg_link_bad_request"}, {"tg_user_id", update.from_id}, {"error", result.error}});
        reply(update, texts::kBadCode);
        return;
    case auth::ExchangeResult::Status::Unavailable:
        applog::error("telegram link exchange failed",
                      {{"event", "tg_link_exchange_failed"}, {"tg_user_id", update.from_id}, {"error", result.error}});
        reply(update, texts::kUnavailable);
        return;
    }

    // Переподключение того же чата: часовой пояс — настройка человека в
    // Telegram, он сохраняется всегда; кошелёк по умолчанию — только если
    // чат подключили к тому же пользователю кошелька (у другого такого
    // кошелька нет). Прежнюю связку в google-auth заменил сам обмен кода.
    auto previous = store_.find_link(update.from_id);
    bool other_user = previous && previous->user_id != result.user_id;

    links::Link link;
    link.tg_user_id = update.from_id;
    link.chat_id = update.chat_id;
    link.user_id = result.user_id;
    link.device_id = result.device_id;
    link.tz = previous ? previous->tz : config_.default_tz;
    if (previous && !other_user) link.default_wallet_uuid = previous->default_wallet_uuid;
    store_.save_link(link);
    // Токен и счета прежнего пользователя в кеше больше не годятся.
    sessions_.forget(update.from_id);
    forget_accounts(update.from_id);

    applog::set_user(result.user_id);
    applog::info("telegram chat linked", {{"event", "tg_linked"},
                                          {"tg_user_id", update.from_id},
                                          {"device_id", result.device_id},
                                          {"relinked", previous.has_value()},
                                          {"other_user", other_user}});
    reply(update, texts::kLinked);
}

bool Bot::with_token(const telegram::Update& update, const links::Link& link,
                     const std::function<bool(const std::string&)>& call) {
    // Второй проход — только если сервис ответил 401 на токен из кеша: берём
    // новый токен принудительно и пробуем ровно ещё раз.
    for (int attempt = 0; attempt < 2; ++attempt) {
        sessions::Access access = sessions_.access_token(link.tg_user_id, /*force_refresh=*/attempt > 0);
        if (access.status == sessions::Access::Status::Revoked) {
            reply(update, texts::kRevoked);
            return false;
        }
        if (access.status == sessions::Access::Status::Unavailable) {
            reply(update, texts::kUnavailable);
            return false;
        }
        if (!call(access.token)) return true;
    }
    return true;
}

std::optional<json> Bot::report_get(const telegram::Update& update, const links::Link& link,
                                    const std::string& path_and_query) {
    reports::Response r;
    bool called = with_token(update, link, [&](const std::string& token) {
        r = reports_.get(path_and_query, token);
        return r.status == 401;
    });
    if (!called) return std::nullopt;
    if (r.status == 200) return r.body;

    applog::warn("wallet-report request failed", {{"event", "tg_report_failed"},
                                                  {"tg_user_id", link.tg_user_id},
                                                  {"status", r.status},
                                                  {"error", r.error}});
    reply(update, texts::kUnavailable);
    return std::nullopt;
}

const resolver::Accounts* Bot::accounts(const telegram::Update& update, const links::Link& link, bool fresh) {
    auto it = accounts_cache_.find(link.tg_user_id);
    if (!fresh && it != accounts_cache_.end() && now() - it->second.fetched_at < kAccountsTtlSecs) {
        return &it->second.accounts;
    }
    auto body = report_get(update, link, "/api/reports/accounts");
    if (!body) return nullptr;
    CachedAccounts& slot = accounts_cache_[link.tg_user_id];
    slot.accounts = resolver::parse_accounts(*body);
    slot.fetched_at = now();
    return &slot.accounts;
}

void Bot::forget_accounts(std::int64_t tg_user_id) { accounts_cache_.erase(tg_user_id); }

std::string Bot::eq_currency_for(const links::Link& link, const std::vector<resolver::Wallet>& wallets) {
    for (const auto& w : wallets) {
        if (w.uuid == link.default_wallet_uuid && !w.iso_code.empty()) return w.iso_code;
    }
    for (const auto& w : wallets) {
        if (!w.iso_code.empty()) return w.iso_code;
    }
    return {};
}

void Bot::handle_wallets(const telegram::Update& update, const links::Link& link) {
    // fresh: кто смотрит список, дальше пишет трату по свежим именам.
    const resolver::Accounts* acc = accounts(update, link, /*fresh=*/true);
    if (acc == nullptr) return;
    const auto* wallets = &acc->wallets;
    if (wallets->empty()) {
        reply(update, texts::kNoWallets);
        return;
    }

    // f_balance_to_date считает обороты строго ДО даты — «на завтра» в поясе
    // пользователя значит «включая все сегодняшние операции».
    std::string date = tz::local_date(link.tz, now(), 1);
    auto balance = report_get(update, link,
                              "/api/reports/balance?date=" + date +
                                  "&eq_currency=" + reports::url_encode(eq_currency_for(link, *wallets)));
    if (!balance) return;

    std::unordered_map<std::string, std::string> by_uuid;
    auto rows = balance->find("accounts");
    if (rows != balance->end() && rows->is_array()) {
        for (const auto& row : *rows) by_uuid[str(row, "account_uuid")] = str(row, "balance");
    }

    std::string out = "Кошельки:\n";
    for (const auto& w : *wallets) {
        auto it = by_uuid.find(w.uuid);
        std::string amount = it != by_uuid.end() && !it->second.empty() ? it->second : "0";
        out += (w.uuid == link.default_wallet_uuid ? "★ " : "• ") + w.name + " — " +
               text::format_amount(amount, std::to_string(w.precision), w.symbol) + "\n";
    }
    if (link.default_wallet_uuid.empty()) out += "\nКошелёк по умолчанию для трат: /default <имя>";
    reply(update, out);
}

void Bot::handle_categories(const telegram::Update& update, const links::Link& link) {
    // Системные категории (Transfer и т.п.) parse_accounts уже отбросил.
    const resolver::Accounts* acc = accounts(update, link, /*fresh=*/true);
    if (acc == nullptr) return;
    std::vector<std::string> names;
    for (const auto& c : acc->categories) names.push_back(c.name);
    if (names.empty()) {
        reply(update, "Категорий пока нет — заведите их в приложении.");
        return;
    }
    std::string out = "Категории (" + std::to_string(names.size()) + "):\n";
    for (const auto& n : names) out += "• " + n + "\n";
    reply(update, out);
}

void Bot::handle_last(const telegram::Update& update, const links::Link& link, const std::string& args) {
    int limit = kLastDefault;
    if (!args.empty()) {
        bool digits = std::all_of(args.begin(), args.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (!digits || args.size() > 3) {
            reply(update, "Использование: /last или /last 20 (не больше " + std::to_string(kLastMax) + ").");
            return;
        }
        limit = std::clamp(std::stoi(args), 1, kLastMax);
    }

    const resolver::Accounts* acc = accounts(update, link, /*fresh=*/false);
    if (acc == nullptr) return;
    const auto* wallets = &acc->wallets;
    if (wallets->empty()) {
        reply(update, texts::kNoWallets);
        return;
    }
    std::string to = tz::local_date(link.tz, now(), 1) + "T00:00:00";
    auto body = report_get(update, link,
                           "/api/reports/operations?from=1970-01-01T00:00:00&to=" + to +
                               "&eq_currency=" + reports::url_encode(eq_currency_for(link, *wallets)) +
                               "&limit=" + std::to_string(limit));
    if (!body) return;

    auto rows = body->find("operations");
    if (rows == body->end() || !rows->is_array() || rows->empty()) {
        reply(update, "Операций пока нет.");
        return;
    }
    std::string out = "Последние операции:\n";
    for (const auto& op : *rows) {
        std::string kind = str(op, "kind");
        std::string amount = str(op, "amount");
        std::string line = text::format_day_month(str(op, "date")) + "  ";
        if (kind == "expense") {
            line += text::format_amount("-" + amount, str(op, "precision"), str(op, "symbol")) + "  " +
                    str(op, "category_name") + " · " + str(op, "wallet_name");
        } else if (kind == "income") {
            line += text::format_amount("+" + amount, str(op, "precision"), str(op, "symbol")) + "  " +
                    str(op, "category_name") + " → " + str(op, "wallet_name");
        } else {
            line += text::format_amount(amount, str(op, "precision"), str(op, "symbol")) + "  " +
                    str(op, "from_name") + " → " + str(op, "to_name");
        }
        std::string comment = str(op, "comment");
        if (!comment.empty()) line += " — " + text::truncate_utf8(comment, 80);
        out += line + "\n";
    }
    reply(update, out);
}

void Bot::handle_default(const telegram::Update& update, const links::Link& link, const std::string& args) {
    const resolver::Accounts* acc = accounts(update, link, /*fresh=*/false);
    if (acc == nullptr) return;
    const auto* wallets = &acc->wallets;
    if (wallets->empty()) {
        reply(update, texts::kNoWallets);
        return;
    }

    std::vector<text::Named> named;
    for (const auto& w : *wallets) named.push_back({w.uuid, w.name});
    auto list = [&named]() {
        std::string s;
        for (const auto& n : named) s += "• " + n.name + "\n";
        return s;
    };

    if (args.empty()) {
        std::string current;
        for (const auto& n : named) {
            if (n.uuid == link.default_wallet_uuid) current = n.name;
        }
        reply(update, (current.empty() ? std::string("Кошелёк по умолчанию не выбран.")
                                       : "Кошелёк по умолчанию: " + current + ".") +
                          "\nВыбрать: /default <имя>\n\n" + list());
        return;
    }

    text::Match m = text::resolve_name(args, named);
    if (m.kind == text::Match::Kind::Found) {
        store_.set_default_wallet(link.tg_user_id, m.found.uuid);
        reply(update, "Кошелёк по умолчанию: " + m.found.name + ".");
    } else if (m.kind == text::Match::Kind::Ambiguous) {
        std::string s = "Подходит несколько кошельков, уточните:\n";
        for (const auto& c : m.candidates) s += "• " + c.name + "\n";
        reply(update, s);
    } else {
        reply(update, "Нет кошелька «" + args + "». Есть:\n" + list());
    }
}

void Bot::handle_tz(const telegram::Update& update, const links::Link& link, const std::string& args) {
    // Дата и время в поясе — чтобы сразу сверить со своими часами, что пояс
    // выбран верно (по одной дате ошибку на пару часов не заметить).
    auto describe = [this](const std::string& zone) {
        std::time_t t = now();
        return "Часовой пояс: " + zone + ". Сейчас там " + text::format_day_month(tz::local_date(zone, t)) + ", " +
               tz::local_time(zone, t) + ".";
    };
    if (args.empty()) {
        reply(update, describe(link.tz) + "\nПоменять: /tz Europe/Moscow (любой пояс IANA).");
        return;
    }
    if (!tz::is_valid_name(args, config_.zoneinfo_dir)) {
        reply(update, "Не знаю пояса «" + args + "». Примеры: Europe/Moscow, Asia/Tbilisi, Europe/Berlin, UTC.");
        return;
    }
    store_.set_tz(link.tg_user_id, args);
    reply(update, describe(args));
}

void Bot::handle_unlink(const telegram::Update& update, const links::Link& link) {
    // Связка живёт в google-auth — удалить её надо там, иначе бот
    // продолжил бы получать токены. google-auth недоступен — не делаем вид,
    // что отключили: локальные настройки остаются, пользователь повторит.
    if (!auth_.unlink(link.tg_user_id)) {
        applog::warn("telegram unlink failed", {{"event", "tg_unlink_failed"}, {"tg_user_id", link.tg_user_id}});
        reply(update, texts::kUnavailable);
        return;
    }
    store_.delete_link(link.tg_user_id);
    sessions_.forget(link.tg_user_id);
    forget_accounts(link.tg_user_id);
    applog::info("telegram chat unlinked", {{"event", "tg_unlinked"}, {"tg_user_id", link.tg_user_id}});
    reply(update, texts::kUnlinked);
}

void Bot::handle_expense(const telegram::Update& update, const links::Link& link) {
    // Справочник нужен до парсера: без имён многословная категория не
    // отделится от комментария.
    const resolver::Accounts* acc = accounts(update, link, /*fresh=*/false);
    if (acc == nullptr) return;

    std::time_t t = now();
    std::string today = tz::local_date(link.tz, t);
    parser::Result parsed =
        parser::parse_strict(update.text, resolver::names_for_parser(*acc), {today, tz::local_time_seconds(link.tz, t)});
    bool ai = ai_ != nullptr && ai_eligible(update.text);
    if (!parsed.ok) {
        // Суммы и текст — не в лог (§3.4): только причина.
        applog::info("expense parse failed", {{"event", "tg_expense_parse_failed"},
                                              {"tg_user_id", link.tg_user_id},
                                              {"reason", parser::reason_name(parsed.error.reason)}});
        count_expense("parse_failed");
        metrics::inc_counter("tg_parse_fail_total", "Трата не разобрана, по причине.", "reason",
                             parser::reason_name(parsed.error.reason));
        // Сумму не разобрал («1,200.50») — пусть попробует ИИ; ноль, дата —
        // заданы явно и явно неверны, ИИ тут не поможет (таблица 9c).
        if (ai && parsed.error.reason == parser::ParseError::Reason::BadAmount) {
            AiFallback f;
            f.kind = AiFallback::Kind::ParseError;
            f.reason = parsed.error.reason;
            try_ai(update, link, f);
            return;
        }
        reply(update, parse_error_text(parsed.error.reason));
        return;
    }
    // Категория не нашлась («350 потратил на кофе» — категория «потратил»):
    // строгий формат сдался, пусть попробует ИИ; не выйдет — прежние кнопки.
    if (ai) {
        resolver::Resolution r = resolver::resolve(parsed.expense, *acc, link.default_wallet_uuid);
        if (r.category.kind == resolver::CategoryPick::Kind::Ask &&
            r.category.why == resolver::CategoryPick::Why::NotFound) {
            AiFallback f;
            f.kind = AiFallback::Kind::StrictDraft;
            f.expense = parsed.expense;
            try_ai(update, link, f);
            return;
        }
    }
    start_draft(update, link, parsed.expense, *acc, /*from_text=*/false, /*edit_message_id=*/0);
}

void Bot::start_draft(const telegram::Update& update, const links::Link& link, const parser::ParsedExpense& e,
                      const resolver::Accounts& acc, bool from_text, std::int64_t edit_message_id) {
    std::time_t t = now();
    std::string today = tz::local_date(link.tz, t);
    resolver::Resolution r = resolver::resolve(e, acc, link.default_wallet_uuid);

    applog::info("expense resolved", {{"event", "tg_expense_resolved"},
                                      {"tg_user_id", link.tg_user_id},
                                      {"source", from_text ? "llm" : "strict"},
                                      {"category", category_why_name(r.category)},
                                      {"wallet", wallet_why_name(r.wallet)}});
    metrics::inc_counter("tg_expense_source_total", "Черновики трат по источнику разбора.", "source",
                         from_text ? "llm" : "strict");

    if (r.wallet.kind == resolver::WalletPick::Kind::Reject) {
        count_expense("rejected");
        if (r.wallet.why == resolver::WalletPick::Why::NoWallets) {
            respond(update, texts::kNoWallets, edit_message_id);
        } else {
            respond(update, "Нет кошелька в " + e.currency_hint + " — конвертации нет. Кошельки — /wallets",
                    edit_message_id);
        }
        return;
    }
    if (r.wallet.kind == resolver::WalletPick::Kind::Found && !resolver::fits_precision(e, r.wallet.found)) {
        count_expense("rejected");
        respond(update, precision_text(r.wallet.found), edit_message_id);
        return;
    }

    drafts::Draft d = make_draft(e, r, acc, link.default_wallet_uuid);
    d.id = std::to_string(update.update_id);
    d.tg_user_id = link.tg_user_id;
    d.user_id = link.user_id;
    d.chat_id = update.chat_id;
    d.created_at = t;
    d.from_text = from_text;
    count_expense(d.step == drafts::Draft::Step::Card ? "resolved" : "ambiguous");
    card::Message m = card::render(d, link.default_wallet_uuid, today, tz::local_date(link.tz, t, -1));
    if (edit_message_id != 0) {
        tg_.edit_message(update.chat_id, edit_message_id, text::truncate_utf8(m.text, kMaxMessageBytes), m.keyboard);
        d.message_id = edit_message_id;
    } else {
        // Сначала сообщение, потом черновик: без message_id править нечего.
        d.message_id = tg_.send_message(update.chat_id, text::truncate_utf8(m.text, kMaxMessageBytes), m.keyboard);
    }
    drafts_.put(std::move(d), t);
}

void Bot::respond(const telegram::Update& update, const std::string& text, std::int64_t edit_message_id) {
    if (edit_message_id != 0) {
        tg_.edit_message(update.chat_id, edit_message_id, text::truncate_utf8(text, kMaxMessageBytes), {});
    } else {
        reply(update, text);
    }
}

void Bot::run_fallback(const telegram::Update& update, const links::Link& link, const AiFallback& fallback,
                       std::int64_t edit_message_id) {
    switch (fallback.kind) {
    case AiFallback::Kind::NotUnderstood:
        respond(update, texts::kAiNotUnderstood, edit_message_id);
        return;
    case AiFallback::Kind::ParseError:
        respond(update, parse_error_text(fallback.reason), edit_message_id);
        return;
    case AiFallback::Kind::StrictDraft: {
        const resolver::Accounts* acc = accounts(update, link, /*fresh=*/false);
        if (acc == nullptr) return; // ответ уже отправлен
        start_draft(update, link, fallback.expense, *acc, /*from_text=*/false, edit_message_id);
        return;
    }
    }
}

void Bot::try_ai(const telegram::Update& update, const links::Link& link, const AiFallback& fallback) {
    std::time_t t = now();
    json input = {{"text", update.text},
                  {"now", {{"date", tz::local_date(link.tz, t)}, {"time", tz::local_time_seconds(link.tz, t)}}},
                  {"default_wallet_uuid", link.default_wallet_uuid}};
    // Повторная доставка того же апдейта — то же задание, LLM второй раз не зовётся.
    std::string key = "tg:" + std::to_string(link.tg_user_id) + ":" + std::to_string(update.update_id);

    ai::CreateResult created;
    bool called = with_token(update, link, [&](const std::string& token) {
        created = ai_->create_parse_expense(token, input, key);
        return created.status == ai::CreateResult::Status::Unauthorized;
    });
    if (!called) return; // пользователю уже ответили (сессия отозвана, google-auth недоступен)
    count_ai_job(ai::to_string(created.status));
    if (created.status != ai::CreateResult::Status::Created) {
        // ИИ выключен, занят или недоступен — ответ, как без него, сразу.
        applog::info("ai job not created", {{"event", "tg_ai_job_not_created"},
                                            {"tg_user_id", link.tg_user_id},
                                            {"status", ai::to_string(created.status)},
                                            {"error", created.error}});
        run_fallback(update, link, fallback, 0);
        return;
    }
    PendingAi p;
    p.job_id = created.job_id;
    p.update = update;
    p.created_at = t;
    p.fallback = fallback;
    p.message_id = tg_.send_message(update.chat_id, texts::kAiParsing, {});
    applog::info("ai job created", {{"event", "tg_ai_job_created"}, {"tg_user_id", link.tg_user_id}, {"job_id", p.job_id}});
    pending_ai_.push_back(std::move(p));
}

void Bot::ai_tick() {
    if (ai_ == nullptr || pending_ai_.empty()) return;
    // Обработка может добавить задание (not_expense -> chat, D3): обходим
    // снятую копию, новые и незавершённые копятся в pending_ai_.
    std::vector<PendingAi> current;
    current.swap(pending_ai_);
    for (auto& p : current) {
        applog::set_trace_id(applog::new_trace_id());
        bool finished = true;
        try {
            finished = process_pending_ai(p);
        } catch (const std::exception& e) {
            // Как с апдейтом: одно упавшее задание не должно вечно крутиться.
            applog::error("ai job processing failed",
                          {{"event", "tg_ai_job_failed"}, {"job_id", p.job_id}, {"error", e.what()}});
        }
        applog::clear_request_context();
        if (!finished) pending_ai_.push_back(std::move(p));
    }
}

bool Bot::process_pending_ai(PendingAi& p) {
    std::time_t t = now();
    auto finish = [&](const char* outcome, const std::string& failure = {}) {
        metrics::inc_counter("tg_ai_outcome_total", "Итог заданий wallet-ai у бота.", "outcome", outcome);
        metrics::observe_duration("tg_ai_wait_seconds", "От «Разбираю…» до итога.", "outcome", outcome,
                                  static_cast<double>(t - p.created_at));
        applog::info("ai job finished", {{"event", "tg_ai_job_finished"},
                                         {"tg_user_id", p.update.from_id},
                                         {"job_id", p.job_id},
                                         {"outcome", outcome},
                                         {"failure", failure},
                                         {"wait_secs", t - p.created_at}});
        return true;
    };

    auto link = store_.find_link(p.update.from_id);
    if (!link) {
        tg_.edit_message(p.update.chat_id, p.message_id, texts::kNotLinked, {});
        return finish("unlinked");
    }
    applog::set_user(link->user_id);
    if (p.kind == PendingAi::Kind::Chat) return process_pending_chat(p, *link, t);
    bool give_up = t - p.created_at >= kAiGiveUpSecs;

    ai::JobResult job;
    std::string token;
    for (int attempt = 0; attempt < 2; ++attempt) {
        sessions::Access access = sessions_.access_token(link->tg_user_id, /*force_refresh=*/attempt > 0);
        if (access.status == sessions::Access::Status::Revoked) {
            tg_.edit_message(p.update.chat_id, p.message_id, texts::kRevoked, {});
            return finish("revoked");
        }
        if (access.status == sessions::Access::Status::Unavailable) {
            if (!give_up) return false; // google-auth мигнул — следующий тик
            run_fallback(p.update, *link, p.fallback, p.message_id);
            return finish("gave_up");
        }
        token = access.token;
        job = ai_->get_job(token, p.job_id);
        if (job.status != ai::JobResult::Status::Unauthorized) break;
    }

    using S = ai::JobResult::Status;
    switch (job.status) {
    case S::Pending:
    case S::Error:
        if (!give_up) return false;
        ai_->cancel_job(token, p.job_id);
        run_fallback(p.update, *link, p.fallback, p.message_id);
        return finish("gave_up", job.error);
    case S::Done: {
        std::string outcome = job.result.value("outcome", std::string{});
        if (outcome == "draft" && job.result.contains("expense") && job.result["expense"].is_object()) {
            const resolver::Accounts* acc = accounts(p.update, *link, /*fresh=*/false);
            if (acc == nullptr) return finish("draft_no_accounts");
            start_draft(p.update, *link, expense_from_json(job.result["expense"]), *acc, /*from_text=*/true,
                        p.message_id);
            return finish("draft");
        }
        // Не трата — вероятно, вопрос без признаков («траты на такси за 2
        // недели») или ответ на уточнение с цифрами («2026-10-01 по
        // 2026-10-31», строгий разбор упал на сумме): в чат тем же
        // сообщением (D3). Ненайденная категория («350 потратил на кофе») —
        // трата, там прежние кнопки.
        if (outcome == "not_expense" && (p.fallback.kind == AiFallback::Kind::NotUnderstood ||
                                         p.fallback.kind == AiFallback::Kind::ParseError)) {
            start_chat(p.update, *link, p.message_id, &p.fallback);
            return finish("not_expense_chat");
        }
        run_fallback(p.update, *link, p.fallback, p.message_id);
        return finish(outcome == "not_expense" ? "not_expense" : "rejected");
    }
    case S::Failed:
        run_fallback(p.update, *link, p.fallback, p.message_id);
        return finish("failed", job.failure);
    case S::Cancelled:
    case S::NotFound:
    case S::Unauthorized:
        run_fallback(p.update, *link, p.fallback, p.message_id);
        return finish(job.status == S::NotFound ? "expired" : (job.status == S::Cancelled ? "cancelled" : "unauthorized"));
    }
    return true;
}

void Bot::start_chat(const telegram::Update& update, const links::Link& link, std::int64_t edit_message_id,
                     const AiFallback* fallback) {
    bool after_not_expense = fallback != nullptr;
    std::time_t t = now();
    json input = {{"text", update.text},
                  {"conversation_id", conversation_for(link.tg_user_id, t)},
                  {"now", {{"date", tz::local_date(link.tz, t)}, {"time", tz::local_time_seconds(link.tz, t)}}}};
    // Повторная доставка апдейта — то же задание. После not_expense ключ
    // tg:<id>:<update_id> уже занят заданием parse_expense — свой суффикс.
    std::string key = "tg:" + std::to_string(link.tg_user_id) + ":" + std::to_string(update.update_id);
    if (after_not_expense) key += ":chat";

    ai::CreateResult created;
    bool called = with_token(update, link, [&](const std::string& token) {
        created = ai_->create_chat(token, input, key);
        return created.status == ai::CreateResult::Status::Unauthorized;
    });
    if (!called) return; // пользователю уже ответили (сессия отозвана, google-auth недоступен)
    metrics::inc_counter("tg_chat_jobs_total", "Создание заданий chat у бота по итогу.", "result",
                         ai::to_string(created.status));
    if (created.status != ai::CreateResult::Status::Created) {
        applog::info("chat job not created", {{"event", "tg_chat_job_not_created"},
                                              {"tg_user_id", link.tg_user_id},
                                              {"status", ai::to_string(created.status)},
                                              {"error", created.error}});
        if (after_not_expense) {
            // Чата нет — ответ, как будто ИИ не помог с тратой.
            run_fallback(update, link, *fallback, edit_message_id);
            return;
        }
        using S = ai::CreateResult::Status;
        const char* text = texts::kChatFailed;
        if (created.status == S::ProviderUnavailable) text = texts::kChatProviderUnavailable;
        else if (created.status == S::Unavailable) text = texts::kChatOff;
        else if (created.status == S::Busy) text = texts::kChatBusy;
        respond(update, text, edit_message_id);
        return;
    }

    PendingAi p;
    p.kind = PendingAi::Kind::Chat;
    p.job_id = created.job_id;
    p.update = update;
    p.created_at = t;
    if (edit_message_id != 0) {
        tg_.edit_message(update.chat_id, edit_message_id, texts::kChatThinking, {});
        p.message_id = edit_message_id;
    } else {
        p.message_id = tg_.send_message(update.chat_id, texts::kChatThinking, {});
    }
    applog::info("chat job created", {{"event", "tg_chat_job_created"},
                                      {"tg_user_id", link.tg_user_id},
                                      {"job_id", p.job_id},
                                      {"conversation_id", input["conversation_id"]},
                                      {"after_not_expense", after_not_expense}});
    pending_ai_.push_back(std::move(p));
}

bool Bot::process_pending_chat(PendingAi& p, const links::Link& link, std::time_t t) {
    auto finish = [&](const char* outcome, const std::string& failure = {}, const std::string& provider = {}) {
        metrics::inc_counter("tg_chat_outcome_total", "Итог заданий chat у бота.", "outcome", outcome);
        metrics::observe_duration("tg_chat_wait_seconds", "От «Думаю…» до итога.", "outcome", outcome,
                                  static_cast<double>(t - p.created_at));
        applog::info("chat job finished", {{"event", "tg_chat_job_finished"},
                                           {"tg_user_id", p.update.from_id},
                                           {"job_id", p.job_id},
                                           {"outcome", outcome},
                                           {"failure", failure},
                                           {"provider", provider},
                                           {"wait_secs", t - p.created_at}});
        return true;
    };
    auto show = [&](const std::string& text) {
        tg_.edit_message(p.update.chat_id, p.message_id, text::truncate_utf8(text, kMaxMessageBytes), {});
    };
    bool give_up = t - p.created_at >= config_.chat_wait_secs;

    ai::JobResult job;
    std::string token;
    for (int attempt = 0; attempt < 2; ++attempt) {
        sessions::Access access = sessions_.access_token(link.tg_user_id, /*force_refresh=*/attempt > 0);
        if (access.status == sessions::Access::Status::Revoked) {
            show(texts::kRevoked);
            return finish("revoked");
        }
        if (access.status == sessions::Access::Status::Unavailable) {
            if (!give_up) return false; // google-auth мигнул — следующий тик
            show(texts::kChatTimeout);
            return finish("gave_up");
        }
        token = access.token;
        job = ai_->get_job(token, p.job_id);
        if (job.status != ai::JobResult::Status::Unauthorized) break;
    }

    using S = ai::JobResult::Status;
    switch (job.status) {
    case S::Pending:
    case S::Error:
        if (!give_up) return false;
        ai_->cancel_job(token, p.job_id); // ошибки отмены глотает клиент
        show(texts::kChatTimeout);
        return finish("gave_up", job.error);
    case S::Done: {
        std::string answer = trim(job.result.value("text", std::string{}));
        std::string provider = job.result.value("provider", std::string{});
        if (answer.empty()) {
            show(texts::kChatFailed);
            return finish("empty", {}, provider);
        }
        // Подпись — кто ответил (спека «Показ ответа»): пользователь видит,
        // куда ушли данные. Обрезается текст, а не подпись.
        std::string signature;
        if (provider == "lmstudio") signature = "\n\n— LM Studio";
        else if (provider == "claude") signature = "\n\n— Claude";
        // truncate_utf8 дописывает «…» (3 байта) сверх предела — иначе
        // повторная обрезка в show срезала бы подпись.
        constexpr std::size_t kEllipsisBytes = 3;
        show(text::truncate_utf8(answer, kMaxMessageBytes - signature.size() - kEllipsisBytes) + signature);
        // Модель переспросила — следующий текст пользователя идёт в тот же диалог.
        // Следующий текст пользователя — вероятно, ответ модели в этом же
        // диалоге. Не только после «?»: модель переспрашивает и
        // повелительно («Укажите, пожалуйста, даты.»).
        chats_[link.tg_user_id].awaiting_reply_until = t + kChatReplyWindowSecs;
        return finish("answer", {}, provider);
    }
    case S::Failed:
        if (job.failure == "tools_unavailable" || job.failure == "auth_expired") show(texts::kChatNoData);
        else if (job.failure == "provider_refused") show(texts::kChatRefused);
        else show(texts::kChatFailed);
        return finish("failed", job.failure);
    case S::Cancelled:
    case S::NotFound:
    case S::Unauthorized:
        show(texts::kChatFailed);
        return finish(job.status == S::NotFound ? "expired" : (job.status == S::Cancelled ? "cancelled" : "unauthorized"));
    }
    return true;
}

std::string Bot::conversation_for(std::int64_t tg_user_id, std::time_t t) {
    ChatState& c = chats_[tg_user_id];
    if (c.seq == 0 || t - c.last_question_at >= kChatIdleSecs) new_conversation(tg_user_id, t);
    c.last_question_at = t;
    // Вопрос задан — окно уточнения израсходовано; новый ответ с «?» откроет его снова.
    c.awaiting_reply_until = 0;
    return "tg-" + std::to_string(tg_user_id) + "-" + std::to_string(c.seq);
}

void Bot::new_conversation(std::int64_t tg_user_id, std::time_t t) {
    ChatState& c = chats_[tg_user_id];
    // Номер не меньше текущего времени (D5): после рестарта бот не попадёт в
    // историю wallet-ai прежнего процесса, а внутри процесса номера растут.
    c.seq = std::max<std::int64_t>(c.seq + 1, static_cast<std::int64_t>(t));
    c.last_question_at = t;
    c.awaiting_reply_until = 0;
}

bool Bot::awaiting_reply(std::int64_t tg_user_id, std::time_t t) const {
    auto it = chats_.find(tg_user_id);
    return it != chats_.end() && t < it->second.awaiting_reply_until;
}

void Bot::handle_new(const telegram::Update& update, const links::Link& link) {
    new_conversation(link.tg_user_id, now());
    applog::info("chat conversation reset", {{"event", "tg_chat_new"}, {"tg_user_id", link.tg_user_id}});
    reply(update, texts::kChatNew);
}

std::optional<ai::SettingsResult> Bot::ai_settings(const telegram::Update& update, const links::Link& link,
                                                   const json* put_body) {
    ai::SettingsResult r;
    bool called = with_token(update, link, [&](const std::string& token) {
        r = put_body != nullptr ? ai_->put_settings(token, *put_body) : ai_->get_settings(token);
        return r.status == ai::SettingsResult::Status::Unauthorized;
    });
    if (!called) return std::nullopt;
    metrics::inc_counter("tg_ai_settings_total", "Запросы бота к /api/ai/settings по итогу.", "result",
                         ai::to_string(r.status));
    if (r.status != ai::SettingsResult::Status::Ok) {
        applog::info("ai settings request", {{"event", "tg_ai_settings"},
                                             {"tg_user_id", link.tg_user_id},
                                             {"method", put_body != nullptr ? "PUT" : "GET"},
                                             {"status", ai::to_string(r.status)},
                                             {"error", r.error}});
    }
    return r;
}

void Bot::handle_ai(const telegram::Update& update, const links::Link& link) {
    auto r = ai_settings(update, link, nullptr);
    if (!r) return; // ответ уже отправлен
    if (r->status != ai::SettingsResult::Status::Ok) {
        reply(update, texts::kUnavailable);
        return;
    }
    AiScreen s = render_ai_screen(r->settings);
    tg_.send_message(update.chat_id, s.text, s.keyboard);
}

void Bot::handle_ai_callback(const telegram::Update& update) {
    bool answered = false;
    auto answer = [&](const std::string& text, bool alert) {
        if (answered || update.callback_id.empty()) return;
        answered = true;
        tg_.answer_callback(update.callback_id, text, alert);
    };
    std::string result = "ok";
    auto action = callback::parse_ai(update.callback_data);
    auto log = [&] {
        metrics::inc_counter("tg_ai_callbacks_total", "Нажатия кнопок /ai по итогу.", "result", result);
        applog::info("ai callback", {{"event", "tg_ai_callback"},
                                     {"tg_user_id", update.from_id},
                                     {"action", action ? callback::ai_kind_name(action->kind) : "-"},
                                     {"provider", action ? action->provider : std::string{}},
                                     {"result", result}});
    };
    // Подделка или мусор — пустой ответ, wallet-ai не вызывается.
    if (!action || ai_ == nullptr) {
        result = "bad_data";
        log();
        answer("", false);
        return;
    }
    // Связка — на каждом нажатии: кнопку могли нажать после /unlink.
    auto link = store_.find_link(update.from_id);
    if (!link) {
        result = "not_linked";
        log();
        answer(texts::kNotLinked, true);
        return;
    }
    applog::set_user(link->user_id);
    auto show = [&](const AiScreen& s) {
        if (update.message_id != 0) {
            tg_.edit_message(update.chat_id, update.message_id, s.text, s.keyboard);
        } else {
            tg_.send_message(update.chat_id, s.text, s.keyboard);
        }
    };

    using K = callback::AiKind;
    using S = ai::SettingsResult::Status;
    std::optional<ai::SettingsResult> r;
    if (action->kind == K::Back) {
        r = ai_settings(update, *link, nullptr);
    } else {
        json body = {{"provider", action->provider}};
        if (action->kind == K::Consent) body["consent"] = true;
        if (action->kind == K::Revoke) body["consent"] = false;
        r = ai_settings(update, *link, &body);
    }
    if (!r) {
        result = "no_session";
        log();
        answer("", false); // пользователю уже ответили сообщением
        return;
    }
    switch (r->status) {
    case S::Ok:
        show(render_ai_screen(r->settings));
        result = action->kind == K::Back ? "back" : "saved";
        answer(action->kind == K::Back ? "" : texts::kAiDone, false);
        break;
    case S::ConsentRequired:
        // Облачный провайдер без согласия — экран согласия; выбор не изменён.
        show(render_ai_consent(action->provider));
        result = "consent_required";
        answer("", false);
        break;
    case S::ProviderUnavailable:
        result = "provider_unavailable";
        answer(texts::kAiProviderNotAvailable, false);
        break;
    case S::Unauthorized:
    case S::Unavailable:
        result = "unavailable";
        answer(texts::kUnavailable, true);
        break;
    }
    log();
}

bool Bot::ai_enabled_for(const links::Link& link) {
    std::time_t t = now();
    if (ai_enabled_ && t - ai_enabled_checked_at_ < 60) return *ai_enabled_;
    sessions::Access access = sessions_.access_token(link.tg_user_id);
    if (access.status != sessions::Access::Status::Ok) return false;
    ai_enabled_ = ai_->enabled(access.token).value_or(false);
    ai_enabled_checked_at_ = t;
    return *ai_enabled_;
}

void Bot::handle_callback(const telegram::Update& update) {
    // Ровно один ответ на callback — иначе у пользователя крутятся часы.
    bool answered = false;
    auto answer = [&](const std::string& text, bool alert) {
        if (answered || update.callback_id.empty()) return;
        answered = true;
        tg_.answer_callback(update.callback_id, text, alert);
    };
    std::string result = "ok";
    std::string action_name = "-";
    std::string draft_id;
    auto log = [&] {
        metrics::inc_counter("tg_callbacks_total", "Нажатия кнопок черновика по итогу.", "result", result);
        applog::info("expense callback", {{"event", "tg_expense_callback"},
                                          {"tg_user_id", update.from_id},
                                          {"draft_id", draft_id},
                                          {"action", action_name},
                                          {"result", result}});
    };

    if (!config_.allowed_user_ids.empty() && config_.allowed_user_ids.count(update.from_id) == 0) {
        answer(texts::kClosedBeta, true);
        return;
    }
    // Пустой chat_type — Telegram не прислал сообщение с кнопкой (слишком
    // старое): черновика у него всё равно уже нет, ниже ответим «устарел».
    if (!update.chat_type.empty() && update.chat_type != "private") {
        answer("", false);
        return;
    }
    // Кнопки /ai — свой строгий разбор и свои проверки (D6).
    if (callback::is_ai(update.callback_data)) {
        handle_ai_callback(update);
        return;
    }
    auto action = callback::parse(update.callback_data);
    if (!action) {
        result = "bad_data";
        log();
        answer("", false);
        return;
    }
    draft_id = action->draft_id;
    action_name = callback::kind_name(action->kind);

    auto link = store_.find_link(update.from_id);
    drafts::Draft* d = drafts_.find(action->draft_id, now());
    // Черновик другого пользователя кошелька (чат переподключили) — такой же
    // устаревший: справочник в нём чужой.
    if (d != nullptr && link && d->tg_user_id == update.from_id && d->user_id != link->user_id) {
        drafts_.erase(action->draft_id);
        d = nullptr;
    }
    if (d == nullptr) {
        result = "stale";
        log();
        if (update.message_id != 0) tg_.edit_message(update.chat_id, update.message_id, texts::kDraftStale, {});
        answer(texts::kDraftStale, false);
        return;
    }
    if (d->tg_user_id != update.from_id) {
        result = "foreign";
        log();
        answer("", false);
        return;
    }
    if (!link) {
        result = "not_linked";
        log();
        answer(texts::kNotLinked, true);
        return;
    }
    applog::set_user(link->user_id);

    std::time_t t = now();
    std::string today = tz::local_date(link->tz, t);
    std::string yesterday = tz::local_date(link->tz, t, -1);
    std::int64_t message_id = update.message_id != 0 ? update.message_id : d->message_id;
    std::string answer_text;
    bool alert = false;
    auto reject = [&](const std::string& why) {
        result = "rejected";
        answer_text = why;
        alert = true;
    };

    // Записано — черновик хранится только ради ответа на повторный клик.
    if (d->step == drafts::Draft::Step::Done && action->kind != callback::Kind::Save) {
        result = "done";
        log();
        answer("Уже записано.", false);
        return;
    }
    // Исход записи неизвестен — менять нельзя, только проверить тем же батчем.
    if (d->attempted && action->kind != callback::Kind::Save) {
        result = "locked";
        log();
        answer("Сначала проверьте, записана ли трата, — «✓ Проверить ещё раз».", true);
        return;
    }
    if (action->kind == callback::Kind::Save) {
        SaveReply saved = save_draft(update, *link, *d, message_id, today, yesterday);
        result = saved.result;
        count_save(saved.result);
        log();
        answer(saved.text, saved.alert);
        return;
    }

    const std::size_t n = static_cast<std::size_t>(action->number);
    switch (action->kind) {
    case callback::Kind::PickCategory:
        if (n >= d->categories.size()) {
            result = "bad_data";
            break;
        }
        d->category = n;
        d->category_page = -1;
        next_step(*d);
        break;
    case callback::Kind::CategoryPage:
        if (d->categories.empty()) break;
        d->category_page = std::min(action->number, card::page_count(*d) - 1);
        d->step = drafts::Draft::Step::Category;
        break;
    case callback::Kind::PickWallet: {
        if (n >= d->wallets.size()) {
            result = "bad_data";
            break;
        }
        const resolver::Wallet& w = d->wallets[n];
        const std::string& hint = d->expense.currency_hint;
        if (d->wallet_strict_currency && !hint.empty() && w.iso_code != hint) {
            reject("Сумма в " + hint + ", а кошелёк «" + w.name + "» — в " + w.iso_code + ". Конвертации нет.");
            break;
        }
        if (!resolver::fits_precision(d->expense, w)) {
            reject(precision_text(w));
            break;
        }
        d->wallet = n;
        d->wallet_by_currency = false;
        d->wallet_strict_currency = false;
        next_step(*d);
        break;
    }
    case callback::Kind::EditCategory:
        d->category_why = resolver::CategoryPick::Why::None;
        d->category_page = 0;
        d->step = drafts::Draft::Step::Category;
        break;
    case callback::Kind::EditWallet:
        d->wallet_why = resolver::WalletPick::Why::None;
        d->wallet_candidates.clear();
        for (std::size_t i = 0; i < d->wallets.size(); ++i) d->wallet_candidates.push_back(i);
        d->wallet_force.reset();
        d->wallet_strict_currency = true;
        d->step = drafts::Draft::Step::Wallet;
        break;
    case callback::Kind::MakeDefault:
        if (!d->wallet) break;
        store_.set_default_wallet(link->tg_user_id, d->wallets[*d->wallet].uuid);
        d->offer_remember = false;
        answer_text = "Кошелёк по умолчанию: " + d->wallets[*d->wallet].name + ".";
        // Карточка ниже рисуется уже без «★».
        link->default_wallet_uuid = d->wallets[*d->wallet].uuid;
        break;
    case callback::Kind::Cancel: {
        std::string text = "✕ Не записано: " + card::summary(*d, today, yesterday);
        drafts_.erase(action->draft_id);
        log();
        tg_.edit_message(update.chat_id, message_id, text::truncate_utf8(text, kMaxMessageBytes), {});
        answer("", false);
        return;
    }
    case callback::Kind::Save:
        break; // выше
    }

    log();
    if (result == "ok") {
        card::Message m = card::render(*d, link->default_wallet_uuid, today, yesterday);
        d->message_id = message_id;
        tg_.edit_message(update.chat_id, message_id, text::truncate_utf8(m.text, kMaxMessageBytes), m.keyboard);
    }
    answer(answer_text, alert);
}

Bot::SaveReply Bot::save_draft(const telegram::Update& update, const links::Link& link, drafts::Draft& d,
                               std::int64_t message_id, const std::string& today, const std::string& yesterday) {
    auto show = [&] {
        card::Message m = card::render(d, link.default_wallet_uuid, today, yesterday);
        d.message_id = message_id;
        tg_.edit_message(update.chat_id, message_id, text::truncate_utf8(m.text, kMaxMessageBytes), m.keyboard);
    };
    // Черновик — в «✕ Не записано …» и удалить: запись этим черновиком
    // невозможна.
    auto give_up = [&](const std::string& why) {
        std::string text = "✕ Не записано: " + card::summary(d, today, yesterday) + " — " + why;
        std::string id = d.id;
        drafts_.erase(id); // d дальше не трогаем
        tg_.edit_message(update.chat_id, message_id, text::truncate_utf8(text, kMaxMessageBytes), {});
    };
    const char* const kLost = "Кошелёк или категорию удалили в приложении — выберите заново.";

    if (d.step == drafts::Draft::Step::Done) return {"Уже записано.", false, "done"};

    // Сверка — только до первой попытки: после неясного исхода содержимое
    // закреплено, повтор идёт тем же батчем как есть.
    if (!d.attempted) {
        const resolver::Accounts* acc = accounts(update, link, /*fresh=*/true);
        if (acc == nullptr) return {"", false, "accounts_unavailable"}; // ответ уже отправлен
        if (refresh_snapshot(d, *acc)) {
            next_step(d);
            show();
            return {kLost, true, "lost"};
        }
    }
    if (!d.category || !d.wallet) {
        // Не с карточки (двойной клик на старой клавиатуре) — показать, что нужно.
        next_step(d);
        show();
        return {"", false, "incomplete"};
    }
    const resolver::Wallet& w = d.wallets[*d.wallet];
    if (!d.attempted && !resolver::fits_precision(d.expense, w)) return {precision_text(w), true, "precision"};

    std::string base = config_.bot_username + ":" + std::to_string(link.tg_user_id) + ":" + d.id;
    wallet::Expense e;
    e.batch_id = uuidgen::v5(kDraftNamespace, base + ":batch");
    e.op_uuid = uuidgen::v5(kDraftNamespace, base + ":op");
    e.date = d.expense.datetime;
    e.wallet_uuid = w.uuid;
    e.category_uuid = d.categories[*d.category].uuid;
    e.amount = d.expense.amount;
    e.comment = d.expense.comment;

    wallet::PushResult r;
    bool called = with_token(update, link, [&](const std::string& token) {
        r = wallet_.push_expense(e, token);
        return r.status == wallet::PushResult::Status::Unauthorized;
    });
    if (!called) return {"", false, "token_unavailable"}; // with_token уже ответил сообщением

    applog::info("expense push", {{"event", "tg_expense_push"},
                                  {"tg_user_id", link.tg_user_id},
                                  {"draft_id", d.id},
                                  {"status", wallet::status_name(r.status)},
                                  {"http_status", r.http_status},
                                  {"seq", r.seq},
                                  {"retry", d.attempted}});

    using S = wallet::PushResult::Status;
    switch (r.status) {
    case S::Ok:
    case S::AlreadyExists:
        d.step = drafts::Draft::Step::Done;
        d.attempted = false;
        show();
        return {"Записано.", false, "saved"};
    case S::Unavailable:
        d.attempted = true;
        show();
        return {"Сервер недоступен — нажмите «Проверить ещё раз» чуть позже.", true, "unavailable"};
    case S::Unauthorized:
        // 401 и на новый токен: не применено точно, но и повтор не поможет.
        applog::error("wallet-api rejects bot token", {{"event", "tg_expense_push_unauthorized"},
                                                       {"tg_user_id", link.tg_user_id}});
        return {texts::kUnavailable, true, "unauthorized"};
    case S::MissingReference: {
        // Счёт исчез между сверкой и записью (удалён насовсем). 409 — батч
        // не применён, можно выбирать заново.
        d.attempted = false;
        forget_accounts(link.tg_user_id);
        const resolver::Accounts* acc = accounts(update, link, /*fresh=*/true);
        if (acc == nullptr) return {"", false, "missing_reference"};
        if (!refresh_snapshot(d, *acc)) {
            d.category.reset(); // какой именно счёт, 409 не говорит — спросим категорию
        }
        next_step(d);
        show();
        return {kLost, true, "missing_reference"};
    }
    case S::Reader:
        give_up("у вас в этом кошельке роль «читатель», записывать нельзя.");
        return {"", false, "reader"};
    case S::Rejected:
        applog::error("wallet-api rejected expense", {{"event", "tg_expense_push_rejected"},
                                                      {"tg_user_id", link.tg_user_id},
                                                      {"http_status", r.http_status},
                                                      {"error", r.error}});
        give_up("ошибка сервиса, внесите трату в приложении.");
        return {"", false, "rejected"};
    }
    return {"", false, "unknown"};
}

void run_poll_loop(telegram::TelegramApi& tg, links::LinkStore& store, Bot& bot, const std::atomic<bool>& stop,
                   int poll_timeout_secs) {
    std::int64_t offset = -1; // -1 — ещё не прочитан из базы
    int backoff_secs = 1;

    auto sleep_interruptible = [&stop](int seconds) {
        for (int i = 0; i < seconds * 10 && !stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    while (!stop.load()) {
        try {
            if (offset < 0) offset = store.load_offset();
            // Ждут итога задания wallet-ai — getUpdates недолгий, чтобы
            // ai_tick правил «Разбираю…» без задержки на весь long poll.
            auto updates = tg.get_updates(offset, bot.has_pending_ai() ? 1 : poll_timeout_secs);
            metrics::set_gauge("tg_last_poll_ok_timestamp_seconds",
                               "Unix-время последнего успешного getUpdates.",
                               static_cast<double>(std::time(nullptr)));
            backoff_secs = 1;

            for (const auto& update : updates) {
                applog::set_trace_id(applog::new_trace_id());
                try {
                    bot.handle(update);
                } catch (const std::exception& e) {
                    metrics::inc_counter("tg_update_failures_total",
                                         "Апдейты, обработка которых упала с исключением (апдейт пропущен).",
                                         "stage", "handle");
                    applog::error("telegram update failed", {{"event", "tg_update_failed"},
                                                             {"update_id", update.update_id},
                                                             {"tg_user_id", update.from_id},
                                                             {"error", e.what()}});
                }
                applog::clear_request_context();
                // Offset в памяти двигается сразу: следующий getUpdates и так
                // подтвердит Telegram этот апдейт. Если save_offset упадёт
                // (база недоступна), в базу он попадёт на следующем апдейте.
                offset = update.update_id + 1;
                store.save_offset(offset);
            }
            bot.ai_tick();
        } catch (const std::exception& e) {
            metrics::inc_counter("tg_poll_errors_total", "Ошибки цикла поллинга (getUpdates или своя база).",
                                 "stage", "poll");
            applog::warn("telegram poll failed, retrying",
                         {{"event", "tg_poll_failed"}, {"error", e.what()}, {"retry_in_secs", backoff_secs}});
            applog::clear_request_context();
            sleep_interruptible(backoff_secs);
            backoff_secs = std::min(backoff_secs * 2, 30);
        }
    }
}

} // namespace bot
