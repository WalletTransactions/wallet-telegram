# Tasks

Изменение не меняет код: задачи — только сверка спецификаций с кодом и
тестами. Найденное несоответствие исправляется в спецификации, если неточна
она, или записывается в «Наблюдения» `design.md`, если расходится документ или
код ведёт себя неожиданно. Код в рамках этого изменения не правится, новые
тесты не пишутся: пробелы покрытия фиксируются для отдельного change.

## 1. Исходное состояние

- [ ] 1.1 Убедиться, что сверка идёт на коммите из `design.md` (`3910255`) или зафиксировать новый коммит в Context; проверка — `git log -1` совпадает с записанным
- [ ] 1.2 Собрать и прогнать юнит-тесты (`cmake -B build -DWALLET_TELEGRAM_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure`); проверка — все тесты зелёные, результат записан в раздел «Покрытие» `design.md`

## 2. Сверка по capability

Для каждой спецификации: каждый сценарий сопоставить с тестом (`tests/*.cpp`,
`tests_integration/test_flow.cpp`) или местом в коде; сценарии без теста
перечислить в разделе «Покрытие» `design.md`.

- [ ] 2.1 `telegram-update-intake` сверена с `bot.cpp` (`handle`, `parse_command`, `run_poll_loop`), `telegram_client.cpp`, `test_bot.cpp`, `test_parsing.cpp`; проверка — у каждого сценария есть ссылка на тест или пометка «нет теста»
- [ ] 2.2 `telegram-linking` сверена с `handle_start`, `handle_unlink`, `with_token`, `sessions.cpp`, `auth_client.cpp` и тестами связывания в `test_bot.cpp`; проверка — как в 2.1
- [ ] 2.3 `wallet-read-commands` сверена с `handle_wallets/categories/last/default/tz`, `/help` и `test_bot.cpp`, `test_text.cpp`; проверка — как в 2.1
- [ ] 2.4 `expense-strict-input` сверена с `wallet-common/expense/strict.cpp`, `test_strict.cpp`, `parse_error_text`; проверка — как в 2.1, примеры сообщений из спецификации дают указанный разбор в `test_strict.cpp` или ручной проверке
- [ ] 2.5 `expense-draft` сверена с `resolver.cpp`, `card.cpp`, `callback.cpp`, `drafts.cpp`, `handle_callback` и `test_resolver.cpp`, `test_callback.cpp`, `test_drafts.cpp`, `test_bot.cpp`; проверка — как в 2.1, тексты кнопок и вопросов совпадают с кодом дословно
- [ ] 2.6 `expense-save` сверена с `save_draft`, `wallet_client.cpp`, `test_wallet_client.cpp`, `test_bot.cpp`, `tests_integration/test_flow.cpp`; проверка — как в 2.1, таблица исходов `/sync/push` совпадает с `parse_push_response`
- [ ] 2.7 `expense-free-text` сверена с `try_ai`, `ai_tick`, `process_pending_ai`, `ai_client.cpp`, `test_ai_client.cpp`, `test_bot.cpp`; проверка — как в 2.1
- [ ] 2.8 `service-operations` сверена с `main.cpp`, `docs.cpp`, `db.cpp::verify_schema`, `.env.example`, `wallet-infra/infra/k8s/09-wallet-telegram.yaml`, `test_metrics.cpp`; проверка — как в 2.1, список переменных окружения совпадает с `main.cpp`

## 3. Итоговая проверка

- [ ] 3.1 Прогнать `openspec validate baseline-wallet-telegram --strict`; проверка — change валиден
- [ ] 3.2 Перечитать раздел «Наблюдения» `design.md` после сверки: новые расхождения добавлены, ошибочные убраны; проверка — каждое Р-наблюдение ссылается на конкретный файл

## Workflow follow-up

- Заархивировать change (`/opsx:archive`) после ревью: спецификации переедут в `openspec/specs/`.
- В `org/docs/TODO_telegram_bot.md` (другой репозиторий) добавить пометку, что реализованное в wallet-telegram описано в `wallet-telegram/openspec/specs/`.
- Решить, какие из Р1–Р9 оформлять отдельными changes.
