# Tasks

## 1. Разбор справочника

- [x] 1.1 Отбрасывать долговые кошельки (необязательное `counterparty_uuid` не `null`) при разборе `/accounts` до кеширования, рядом с фильтром системных категорий (design D1–D2). Проверка: юнит-тест разбора — кошелёк с `counterparty_uuid` отброшен, без поля — оставлен, системные категории по-прежнему отброшены.
- [x] 1.2 Проверить, откуда wallet-ai получает список кошельков для черновика траты; если от бота — убедиться, что он отфильтрован. Проверка: тест или запись в PR с путём в коде.

## 2. Команды

- [x] 2.1 `/wallets`, выбор кошелька при вводе траты, `/default`, сопоставление имён: тесты на сценарии `specs/wallet-read-commands` — долговой кошелёк не виден, не предлагается, не принимается в `/default`. Проверка: `ctest` зелёный.
- [x] 2.2 Сброс сохранённого `default_wallet`, указывающего на долговой кошелёк. Проверка: юнит-тест — бот ведёт себя как при удалённом кошельке по умолчанию.

## 3. Выкат

- [ ] 3.1 Проверка на test после wallet-report с `counterparty_uuid` в `/accounts`: в `/wallets` долговых кошельков нет. Проверка: ручной прогон в боте на test-леджере с долговым кошельком.

## Примечания к выполнению

- 1.1 — фильтр в `resolver::parse_accounts` общего кода wallet-common (change `debts` wallet-common, 0.1.1), копия обновлена `vendor.sh` во всех сервисах; в боте правок кода нет — копию в `third_party/` править нельзя (design D1). Тест — `tests/test_resolver.cpp`, `resolver_parse_accounts_skips_debt_wallets_and_system_categories`: `counterparty_uuid` строкой — отброшен, `null` и без поля — оставлен, Transfer отброшен, имя долгового кошелька не попадает в `names_for_parser`.
- 1.2 — wallet-ai берёт справочник сам: `src/ai.cpp` (`GET /api/reports/accounts`), черновик — `src/parse_expense.cpp`, импорт — `src/imports.cpp`, `src/import_runner.cpp`; везде `resolver::parse_accounts`, то есть фильтр общий. wallet-mcp (`src/tools.cpp`) — так же. Тесты wallet-ai (142) и wallet-mcp — зелёные с новой копией.
- 2.1, 2.2 — `tests/test_bot.cpp`, `debt_wallet_hidden_from_wallets_default_and_expense`: `/wallets` без «Вася · USD», `/default` и `/default вася` его не видят и не сохраняют, трата без кошелька спрашивает среди обычных; сохранённый долговой кошелёк по умолчанию — как удалённый (resolver считает его не заданным, в `/wallets` нет «★» и, как при удалённом, нет подсказки `/default`; сама запись в базе не стирается — так же, как у удалённого). `ctest`: все юнит-тесты зелёные (сборка в `debian:bookworm-slim` с tzdata).

## Workflow follow-up

- Архивировать change после выката на test и проверки 3.1 (`openspec archive`), не дожидаясь prod — решение владельца 2026-11-12.
