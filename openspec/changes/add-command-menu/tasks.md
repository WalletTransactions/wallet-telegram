# Tasks

## 1. Меню

- [x] 1.1 `TelegramApi::set_my_commands` (`setMyCommands`), `bot::menu_commands(ai_enabled)`, вызов при старте с перехватом ошибки. Проверка: юнит-тест — состав с ИИ и без, каждая команда меню есть в `/help`, лимиты Bot API (`[a-z0-9_]{1,32}`, описание 1..256) — `tests/test_bot.cpp`: `menu_commands_match_help_and_api_limits`; интеграционные тесты и сервис собираются с новым методом фейка
- [ ] 1.2 На test: после выката в журнале `tg_commands_set` с `ai: true`, в клиенте Telegram по «/» видны команды с `/ai` и `/new`
