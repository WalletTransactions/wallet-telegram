## ADDED Requirements

### Requirement: Меню команд
При старте бот SHALL регистрировать в Telegram (`setMyCommands`) меню команд с
краткими описаниями: `/wallets`, `/last`, `/categories`, `/default`, `/tz`,
`/help`, `/unlink`, а при заданном `WALLET_AI_URL` ещё `/ai` и `/new`.
`/start` в меню не входит. Ошибка регистрации SHALL записываться в журнал
предупреждением и не останавливать бота.

#### Scenario: ИИ включён
- **WHEN** бот стартует с заданным `WALLET_AI_URL`
- **THEN** по «/» в клиенте Telegram видны команды из `/help`, включая `/ai` и `/new`

#### Scenario: ИИ выключен
- **WHEN** бот стартует без `WALLET_AI_URL`
- **THEN** в меню нет `/ai` и `/new`

#### Scenario: Telegram недоступен на старте
- **WHEN** `setMyCommands` падает
- **THEN** в журнале предупреждение `tg_commands_set_failed`, бот продолжает работу
