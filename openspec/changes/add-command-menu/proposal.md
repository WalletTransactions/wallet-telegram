# Proposal

## Why

В Telegram по «/» и кнопке «Меню» список команд не появляется: бот не
регистрирует их (`setMyCommands`). Команды знает только тот, кто прочитал
`/help`. С `add-ai-chat` команд стало больше (`/ai`, `/new`).

## What Changes

- При старте бот вызывает `setMyCommands` (scope по умолчанию) со списком
  команд из `/help`: `/wallets`, `/last`, `/categories`, `/ai`, `/new`,
  `/default`, `/tz`, `/help`, `/unlink`. `/ai` и `/new` — только при
  заданном `WALLET_AI_URL`. `/start` в меню нет.
- Ошибка `setMyCommands` не мешает старту: предупреждение в журнале, меню
  остаётся прежним.

## Capabilities

### Modified Capabilities

- `telegram-update-intake`: новое требование «Меню команд».

## Impact

- **Код:** `TelegramApi::set_my_commands`, `bot::menu_commands`, вызов в
  `main.cpp`.
- **Конфиг:** без новых переменных.
