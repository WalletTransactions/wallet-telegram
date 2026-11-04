// Клиент Telegram Bot API (org/docs/TODO_telegram_bot.md, §1.3, §1.5).
//
// Транспорт — long polling (getUpdates): боту нужен только исходящий HTTPS до
// api.telegram.org, без публичного адреса, TLS-сертификата и входящего порта.
// Интерфейс TelegramApi, а не сразу HTTP-класс, — чтобы Bot (bot.hpp) в
// юнит-тестах получал фейк и не ходил в сеть.
//
// Токен бота входит в сам путь запроса (/bot<token>/<method>) — поэтому ни
// путь, ни URL запроса к Telegram нигде не логируются, только имя метода.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace telegram {

// Кнопка inline-клавиатуры. data — callback_data, 1..64 байта (лимит Bot
// API); текст кнопки режется до kMaxButtonTextBytes.
struct Button {
    std::string text;
    std::string data;
};
// Ряды кнопок сверху вниз. Пустая — без клавиатуры (а в edit_message —
// убрать клавиатуру).
using Keyboard = std::vector<std::vector<Button>>;

constexpr std::size_t kMaxCallbackDataBytes = 64;
// Лимита на текст кнопки у Bot API нет, клиент сам обрежет по ширине; 64
// байта — ~30 символов кириллицы, «Всё равно в «Тинькофф рубли»» влезает.
constexpr std::size_t kMaxButtonTextBytes = 64;

// Только то, что нужно боту сейчас: сообщение или нажатие inline-кнопки
// (callback_query). Апдейты других типов (edited_message, channel_post, ...)
// разбираются с has_message == has_callback == false и просто сдвигают
// offset.
struct Update {
    std::int64_t update_id = 0;
    bool has_message = false;
    bool has_callback = false;
    std::int64_t chat_id = 0;
    std::string chat_type; // "private" / "group" / "supergroup" / "channel"
    std::int64_t from_id = 0;
    std::string from_username; // без '@', может быть пустым
    std::string text;          // пусто у стикеров, фото и т.п.
    // Сообщение: его id. Callback: id сообщения с нажатой кнопкой; 0 —
    // Telegram его не прислал (сообщение недоступно), тогда chat_id = from_id,
    // chat_type пуст и править нечего.
    std::int64_t message_id = 0;
    std::string callback_id;   // для answer_callback
    std::string callback_data; // недоверенные данные от клиента
};

// Ошибка вызова Bot API: сеть, HTTP не 200 или {"ok": false}. description —
// текст от Telegram либо описание сетевой ошибки; токена в нём нет.
// Команда в меню бота: command — без «/», [a-z0-9_]{1,32}; description —
// 1..256 символов (лимиты Bot API).
struct BotCommand {
    std::string command;
    std::string description;
};

struct ApiError : std::runtime_error {
    std::string method;
    int http_status; // 0 — до HTTP-ответа не дошло (сеть, таймаут)

    ApiError(std::string method_in, int status, const std::string& description)
        : std::runtime_error("telegram " + method_in + ": " + description),
          method(std::move(method_in)),
          http_status(status) {}
};

class TelegramApi {
public:
    virtual ~TelegramApi() = default;

    // offset — update_id первого ещё не обработанного апдейта (Telegram
    // считает все апдейты с меньшим id подтверждёнными). timeout_secs —
    // сколько сервер Telegram держит запрос, если апдейтов нет.
    virtual std::vector<Update> get_updates(std::int64_t offset, int timeout_secs) = 0;

    // Простой текст, без parse_mode: пользовательские имена категорий и
    // кошельков попадут в ответы бота дословно, а Markdown/HTML пришлось бы
    // экранировать в каждом месте. Возвращает message_id отправленного —
    // сообщение с кнопками потом правится через edit_message.
    //
    // Аргументов по умолчанию у виртуальных методов нет (переопределения их
    // не наследуют) — пустую клавиатуру вызывающий передаёт явно.
    virtual std::int64_t send_message(std::int64_t chat_id, const std::string& text, const Keyboard& keyboard) = 0;

    // Новый текст и клавиатура сообщения бота; пустая keyboard убирает
    // клавиатуру. «message is not modified» (повторный клик той же кнопки) —
    // не ошибка, глотается.
    virtual void edit_message(std::int64_t chat_id, std::int64_t message_id, const std::string& text,
                              const Keyboard& keyboard) = 0;

    // Ответ на нажатие кнопки — ровно один на callback, иначе у пользователя
    // ~15 с крутятся часы. Пустой text — просто погасить их; alert — окно с
    // кнопкой «OK» вместо всплывающей подсказки.
    virtual void answer_callback(const std::string& callback_id, const std::string& text, bool alert) = 0;

    // Меню команд, которое клиент Telegram показывает по «/» и кнопкой
    // «Меню» (setMyCommands, scope по умолчанию — все личные чаты).
    // Заменяет прежний список целиком.
    virtual void set_my_commands(const std::vector<BotCommand>& commands) = 0;
};

// reply_markup для sendMessage/editMessageText — чистая функция ради
// юнит-тестов: {"inline_keyboard": [[{"text", "callback_data"}, ...], ...]}.
// Текст кнопки режется до kMaxButtonTextBytes по границе UTF-8. data пустая
// или длиннее kMaxCallbackDataBytes — std::invalid_argument: такие данные
// бот формирует сам, это ошибка в коде, а не во вводе.
nlohmann::json keyboard_json(const Keyboard& keyboard);

// Разбор тела ответа getUpdates ({"ok": true, "result": [...]}) — чистая
// функция ради юнит-тестов. Бросает ApiError("getUpdates", 200, ...), если
// ok != true или result не массив.
std::vector<Update> parse_updates(const nlohmann::json& body);

// base_url — "https://api.telegram.org" (TELEGRAM_API_URL в .env позволяет
// подменить его, например, на локальный Bot API server).
std::unique_ptr<TelegramApi> make_http_api(const std::string& base_url, const std::string& bot_token);

} // namespace telegram
