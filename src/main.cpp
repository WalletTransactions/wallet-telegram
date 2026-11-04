// wallet-telegram — Telegram-бот для ввода трат (org/docs/TODO_telegram_bot.md).
//
// Доступ бота к кошельку — связка tg_user_id → user_id в google-auth
// (org/docs/TODO_telegram_tokens.md): на каждую команду бот берёт короткий
// токен (5 мин, scope reports:read expense:write) и ходит с ним в
// wallet-report/wallet-api. Своих прав на базу кошелька у него нет;
// собственная база (telegram) хранит только настройки чатов и offset
// поллинга, секретов в ней нет.
//
// Два потока:
//   - главный — цикл long polling Telegram (bot.hpp::run_poll_loop), апдейты
//     строго по порядку;
//   - фоновый — httplib только для служебных ручек (/healthz, /version,
//     /metrics, /openapi.yaml, /docs). Публичного API у бота нет.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#include "auth_client.hpp"
#include "bot.hpp"
#include "db.hpp"
#include "docs.hpp"
#include "httplib.h"
#include "link_repo.hpp"
#include "clients/reports_client.hpp"
#include "telegram_client.hpp"
#include "util/dotenv.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"
#include "version.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) { g_stop.store(true); }

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

std::string env_required(const char* key, const std::string& hint) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        applog::error(std::string(key) + " must be set — " + hint + " (см. README.md)");
        std::exit(1);
    }
    return std::string(v);
}

bool env_bool(const char* key, bool fallback) {
    const char* v = std::getenv(key);
    if (v == nullptr) return fallback;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

std::string strip_trailing_slash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

// "211959201, 42" -> {211959201, 42}. Нечисловой элемент — ошибка конфига,
// а не молча пропущенный id: опечатка в allowlist иначе незаметно закрыла бы
// бот для владельца.
std::unordered_set<std::int64_t> parse_id_list(const std::string& csv) {
    std::unordered_set<std::int64_t> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                   item.end());
        if (item.empty()) continue;
        if (!std::all_of(item.begin(), item.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            applog::error("TELEGRAM_ALLOWED_USER_IDS: не число: " + item);
            std::exit(1);
        }
        out.insert(std::stoll(item));
    }
    return out;
}

std::string default_log_dir() {
#ifdef _WIN32
    return "logs";
#else
    return "";
#endif
}

} // namespace

int main() {
    dotenv::load(".env");

    std::string log_dir = env_or("LOG_DIR", default_log_dir());
    if (log_dir == "stdout" || log_dir == "-") log_dir.clear();
    applog::init(log_dir, "wallet-telegram");
    metrics::set_build_info(version::kService, version::kVersion, version::kBuild,
                            version::kGitCommit);

    std::string bot_token = env_required("TELEGRAM_BOT_TOKEN", "токен бота от @BotFather");
    std::string service_secret = env_required(
        "TELEGRAM_SERVICE_SECRET", "общий секрет с google-auth (тот же TELEGRAM_SERVICE_SECRET в google-auth/.env)");
    // TELEGRAM_TOKEN_ENC_KEY больше не нужен: бот не хранит токенов (шаг 6
    // TODO_telegram_tokens.md). Оставшаяся в .env строка просто игнорируется.
    std::string auth_service_url = strip_trailing_slash(
        env_required("AUTH_SERVICE_URL", "адрес google-auth напрямую, не через гейтвей, например http://127.0.0.1:8080"));
    std::string wallet_report_url = strip_trailing_slash(env_required(
        "WALLET_REPORT_URL", "адрес wallet-report напрямую, не через гейтвей, например http://127.0.0.1:8084"));
    std::string wallet_api_url = strip_trailing_slash(env_required(
        "WALLET_API_URL", "адрес wallet-api напрямую, не через гейтвей, например http://127.0.0.1:8082"));
    std::string database_url = env_required(
        "DATABASE_URL", "база бота, например postgresql://wallet_telegram:...@localhost:5432/wallet_telegram");

    bot::Config config;
    config.bot_username = env_or("TELEGRAM_BOT_USERNAME", "");
    config.default_tz = env_or("TELEGRAM_DEFAULT_TZ", "UTC");
    config.allowed_user_ids = parse_id_list(env_or("TELEGRAM_ALLOWED_USER_IDS", ""));
    int poll_timeout_secs = std::stoi(env_or("TELEGRAM_POLL_TIMEOUT_SECS", "25"));
    // Ожидание ответа на вопрос к ИИ (add-ai-chat, D4): дедлайн chat у
    // wallet-ai 120 с плюс очередь.
    config.chat_wait_secs = std::stol(env_or("TG_CHAT_WAIT_SECS", "150"));

    // Одно соединение, не пул: к базе ходит только поток поллинга, по одному
    // апдейту за раз.
    std::unique_ptr<db::Database> database;
    try {
        database = std::make_unique<db::Database>(database_url, env_bool("LOG_SQL_PARAMS", false));
        database->verify_schema();
    } catch (const std::exception& ex) {
        applog::error(std::string("Postgres: ") + ex.what());
        return 1;
    }

    links::PgLinkStore store(*database);
    auto tg = telegram::make_http_api(strip_trailing_slash(env_or("TELEGRAM_API_URL", "https://api.telegram.org")),
                                      bot_token);
    auto auth_api = auth::make_http_api(auth_service_url, service_secret);
    auto reports_api = reports::make_http_api(wallet_report_url, "tg_reports_errors_total");
    auto wallet_api = wallet::make_http_api(wallet_api_url);
    bot::Bot bot(*tg, *auth_api, *reports_api, *wallet_api, store, config);
    // Свободный текст через wallet-ai (org/docs/TODO_wallet_ai.md, шаг 4).
    // Пусто — ИИ выключен, бот работает как без него.
    std::string wallet_ai_url = strip_trailing_slash(env_or("WALLET_AI_URL", ""));
    std::unique_ptr<ai::AiApi> ai_api;
    if (!wallet_ai_url.empty()) {
        ai_api = ai::make_http_api(wallet_ai_url);
        bot.enable_ai(ai_api.get());
    }
    // Меню команд по «/» (add-command-menu). Telegram недоступен на старте —
    // не повод не стартовать: меню останется прежним, бот работает.
    try {
        tg->set_my_commands(bot::menu_commands(ai_api != nullptr));
        applog::info("telegram command menu set", {{"event", "tg_commands_set"}, {"ai", ai_api != nullptr}});
    } catch (const std::exception& ex) {
        applog::warn(std::string("setMyCommands: ") + ex.what(), {{"event", "tg_commands_set_failed"}});
    }

    std::string host = env_or("HOST", "127.0.0.1");
    int port = std::stoi(env_or("PORT", "8086"));
    httplib::Server svr;
    docsapi::register_routes(svr);
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        // Только служебные ручки, поэтому route — сам путь (кардинальность
        // ограничена списком из docs.cpp) и без access-лога: /healthz и
        // /metrics дёргаются каждые 10–15 с и лишь зашумили бы логи.
        metrics::observe_http_request(req.method, req.path, res.status, 0);
    });
    if (!svr.bind_to_port(host, port)) {
        applog::error("не удалось занять " + host + ":" + std::to_string(port) + " (порт занят?)");
        return 1;
    }
    std::thread http_thread([&svr] { svr.listen_after_bind(); });

    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    applog::info("wallet-telegram v" + std::string(version::kBuild) + " (" + std::string(version::kGitCommit) +
                     ") polling Telegram, служебные ручки на http://" + host + ":" + std::to_string(port),
                 {{"event", "started"},
                  {"bot_username", config.bot_username},
                  {"allowlist_size", config.allowed_user_ids.size()},
                  {"auth_service_url", auth_service_url},
                  {"wallet_report_url", wallet_report_url},
                  {"wallet_api_url", wallet_api_url},
                  {"wallet_ai_url", wallet_ai_url}});

    bot::run_poll_loop(*tg, store, bot, g_stop, poll_timeout_secs);

    applog::info("wallet-telegram stopping", {{"event", "stopping"}});
    svr.stop();
    http_thread.join();
    return 0;
}
