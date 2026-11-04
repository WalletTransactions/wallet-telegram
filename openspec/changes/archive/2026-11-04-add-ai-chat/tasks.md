# Tasks

## 1. Маршрутизация

- [x] 1.1 `src/chat_routing.{hpp,cpp}`: `looks_like_question` по D1 (регистр, `ё`, первое слово, «?», 1000 байт). Проверка: юнит-тесты — «сколько за няню», «Сколько потратил в сентябре 2026», «итого?», «няня 3000» (не вопрос), «что-то 300» (не вопрос), «потратил 300 на кофе» (не вопрос) — `tests/test_chat_routing.cpp`: `question_words_and_question_mark`, `expenses_are_not_questions`, `question_length_limit`, `first_word_normalized`; первое слово берётся после `text::normalize_name` (wallet-common), дефис слово не обрывает
- [x] 1.2 Встроить ветки в `Bot::handle` по D2 (уточнение → вопрос → свободный текст), команды `/ai` и `/new` в список команд со связкой. Проверка: тесты бота на фейках — «сколько за няню?» без связки → предложение подключиться; без `WALLET_AI_URL` → «Пока не понимаю…»; «няня 3000» → `parse_expense` — `tests/test_bot.cpp`: `chat_routing_questions_vs_expenses` (без связки → kNotLinked, без WALLET_AI_URL → kUnknown и `/new` непонятое, «няня 3000» → `parse_expense`, «350 продукты» → строгий путь)

## 2. Задание chat и ожидание

- [x] 2.1 `AiApi::create_chat`, статус `ProviderUnavailable`, чистый разбор ответов (D7). Проверка: юнит-тесты разбора 202, 503 `provider_unavailable`, 503 `unavailable`, 429, 400 — `tests/test_ai_client.cpp`: `ai_create_chat_response` (202, 503 `provider_unavailable`, 503 `unavailable`, 429, 400), `ai_create_response`
- [x] 2.2 `PendingAi.kind` и предел `TG_CHAT_WAIT_SECS` (150) (D4); отправка «Думаю…», показ `result.text` с подписью провайдера, тексты сбоев по спеке. Проверка: тесты с фейковым `AiApi` — `done` → текст + «\n\n— LM Studio»; `failed tools_unavailable` → «Не удалось получить данные кошелька…»; зависшее задание → отмена и «ИИ не успел ответить…» — `tests/test_bot.cpp`: `chat_answer_with_provider_signature` (подпись LM Studio/Claude, длинный ответ режется, подпись остаётся), `chat_failures_and_timeout` (`tools_unavailable`/`auth_expired` → «Не удалось получить данные…», `provider_refused`, прочие; на 150 с — отмена и «ИИ не успел…»; предел chat свой, не 90 с), `chat_not_created_answers_immediately`
- [x] 2.3 `not_expense` → chat в том же сообщении (D3), запасной ответ, если чат не создан. Проверка: тесты — «Разбираю…» → «Думаю…» → ответ; при 503 на `chat` → «Не понял. Пример: …» — `tests/test_bot.cpp`: `chat_after_not_expense_in_same_message` (ключ `…:chat`, одно сообщение «Разбираю…» → «Думаю…» → ответ), `ai_rejected_not_expense_expired_bad_amount` (503 на `chat` → «Не понял…»). `ai_tick` обходит снятую копию: задание chat, созданное во время обхода, не теряется

## 3. Диалог

- [x] 3.1 `ChatState` по D5: `conversation_id`, сброс через 1800 с, `/new` с ответом «Начали новый разговор.», окно уточнения 600 с после ответа с «?». Проверка: тесты с фейковыми часами — два вопроса за 5 минут с одним id; после `/new` id новый; «сентябрь» через минуту после «За какой месяц?» уходит в `chat`, через 15 минут — по обычной маршрутизации — `tests/test_bot.cpp`: `chat_dialog_conversation_and_clarification`, `chat_conversation_ids_survive_restart`; номер разговора = max(прежний + 1, текущее время в секундах)

- [x] 3.2 Окно уточнения после любого ответа чата; текст без цифр в окне — в chat; `not_expense` при неразобранной сумме — тоже в chat (D2, D3, D5 — по живой переписке на test 2026-11-04: ответы модели без «?», «сентябрь» → «Пока не понимаю…», «2026-10-01 по 2026-10-31» → «Не понял сумму»). Проверка: `tests/test_bot.cpp`: `chat_reply_window_after_imperative_answer` (та же переписка), `chat_dialog_conversation_and_clarification`

## 4. /ai и согласие

- [x] 4.1 `AiApi::get_settings`/`put_settings` с разбором ответов. Проверка: юнит-тесты разбора 200, 400 `provider_unavailable`, 409 `consent_required`, 503 — `tests/test_ai_client.cpp`: `ai_settings_response` (200, 400 `provider_unavailable`, 409 `consent_required`, 401, 503, битый 200). Вместо `optional<json>`/`{status, json}` из D7 — общий `SettingsResult` для GET и PUT (повтор на 401 тот же)
- [x] 4.2 `callback::parse_ai` (D6): строгий формат `ai:sel|consent|revoke:<provider>`, `ai:back`. Проверка: юнит-тесты — допустимые варианты, `ai:sel:../../admin`, пустой провайдер, лишние поля — `tests/test_callback.cpp`: `callback_ai_strict_format`
- [x] 4.3 Экран `/ai`, выбор кнопкой, экран согласия с «Согласен»/«Отмена», «Отозвать согласие», проверка связки на каждом нажатии. Проверка: тесты бота на фейках по сценариям `ai-provider-choice`; ручной прогон в тестовом боте — тесты на фейках есть (`ai_screen_render`, `ai_command_shows_screen_or_unavailable`, `ai_choose_claude_with_consent_then_revoke`, `ai_callback_untrusted_and_link_checked`); ручной прогон в тестовом боте не выполнялся. Провайдер из кнопки не сверяется с GET перед PUT (D6): незнакомый отклоняет wallet-ai `400 provider_unavailable` → «Сейчас недоступен»; ручной прогон в тестовом боте 2026-11-04 (0.2.0-rc.2): `/ai` — «Сейчас отвечает: LM Studio», кнопка «✓ LM Studio», нажатие — «Готово»; кнопки Claude нет (на test `not_configured`). Согласие и отзыв на живом Claude — в wallet-ai `enable-claude-provider` 2.2

## 5. Справка и документация

- [x] 5.1 `/help`: строки про вопросы и команды `/ai`, `/new` при включённом ИИ. Проверка: тест справки при `enabled: true`/`false` — `tests/test_bot.cpp`: `help_lists_questions_and_ai_commands_when_enabled`, `ai_help_line_only_when_enabled`
- [x] 5.2 README: раздел «Вопросы к ИИ» (признаки вопроса, уточнение, `/new`, `/ai`, согласие, `TG_CHAT_WAIT_SECS`). Проверка: описанные в README примеры повторяются в тестовом боте — раздел «Вопросы к ИИ» в README написан, метрики и файлы — в таблицах; примеры в тестовом боте не повторялись; в тестовом боте 2026-11-04 повторены: вопрос «сколько за няню?», уточнение словами и датами, `/new`, `/ai`, меню команд

## 6. Сквозная проверка на test-контуре

- [x] 6.1 После выката wallet-ai `add-chat-agent`: в тестовом боте «сколько за няню?» → уточнение → «сентябрь» → сумма. Проверка: сумма совпадает с `/api/reports/expense-structure` за сентябрь, подпись «— LM Studio» — проверено 2026-11-04 на test (бот 0.2.0-rc.2, wallet-ai 0.2.0-rc.2 с chat-v2, wallet-mcp 0.2.1-rc.1): `/new` → «сколько за няню?» → модель спросила период → «давай прошлый месяц» → «Baby Sitter за октябрь 2026?» → «да» → «с 2026-10-01 по 2026-10-31 составил 2884.50 EUR. — LM Studio», совпадает с `expense-structure` за октябрь (`eq_currency=EUR`); все сообщения — в одном `conversation_id`. Первый прогон на rc.1 нашёл: ответы модели без «?» не открывали окно уточнения — исправлено (задача 3.2); «прошлый месяц» = сентябрь и список всех категорий — исправлено в wallet-mcp `chat-period-hints`
- 6.2 `/ai` → «Claude» → согласие — перенесено в wallet-ai change `enable-claude-provider` (задача 2.2): ключа Anthropic API нет, на test `claude` `not_configured`

## Workflow follow-up

- Обновить `org/docs/TODO_telegram_bot.md` (§4a, агентный цикл и маршрутизация) по итогам.
- Архивировать после сквозной проверки.
