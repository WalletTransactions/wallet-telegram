# wallet-telegram

Telegram-бот для ввода трат. Полный план — [../TODO_telegram_bot.md](https://github.com/WalletTransactions/.github-private/blob/main/docs/TODO_telegram_bot.md).

Доступ бота к кошельку — связка `tg_user_id → user_id` в google-auth
(`telegram_links`, [../TODO_telegram_tokens.md](https://github.com/WalletTransactions/.github-private/blob/main/docs/TODO_telegram_tokens.md)). На
каждую команду бот берёт у google-auth короткий токен по `tg_user_id`
(`POST /internal/telegram/token`: 5 минут, `aud` wallet-report + wallet-api,
`scope` `reports:read expense:write`) и ходит с ним в wallet-report. Токенов
пользователя бот не хранит, секретов в его базе нет. Прав на базу кошелька у
бота нет. Отключить бота можно из веба: «Доступ и приглашения → Telegram-бот» →
«Отключить», или командой `/unlink`.

## Что уже работает

- Long polling Telegram (`getUpdates`), апдейты по порядку, offset в своей базе.
- `/start <код>` — связывание: код из веба (Доступ и приглашения → Telegram-бот) меняется на
  связку через `POST /internal/telegram/exchange` google-auth. Переподключение
  чата к другому пользователю заменяет связку в google-auth.
- `/start` без кода, `/help`.
- Команды чтения — данные из wallet-report с токеном пользователя:

  | Команда | Что делает |
  | --- | --- |
  | `/wallets` | кошельки с балансом на сейчас (в поясе пользователя), ★ — кошелёк по умолчанию |
  | `/categories` | категории без системных (Transfer и т.п.) |
  | `/last [N]` | последние N операций (10 по умолчанию, до 30) |
  | `/default <кошелёк>` | кошелёк по умолчанию для трат: точное имя или уникальный префикс, без учёта регистра, ё = е |
  | `/tz <пояс>` | часовой пояс IANA, проверяется по tzdata |
  | `/unlink` | удаление связки в google-auth (`DELETE /internal/telegram/links/{id}`) и локальных настроек |

- Токены: токен бота кешируется в памяти до минуты до истечения. Связку
  удалили из веба — google-auth отвечает 404, бот удаляет локальные настройки
  и просит подключиться заново.
- Только личные чаты: группы бот игнорирует молча.
- Allowlist `TELEGRAM_ALLOWED_USER_IDS` для закрытого тестирования.

- Трата сообщением (шаг 6): строгий формат
  `<сумма> [<валюта>] <категория> [@<кошелёк>] [<дата>] [// <комментарий>]`
  разбирается, кошелёк и категория разрешаются по именам (точно, уникальный
  префикс, кошелёк — с учётом валюты). Что неясно — бот спрашивает кнопками
  в том же сообщении (кандидаты, «Другая…» со страницами, «Всё равно в …»),
  дальше — карточка «350,00 ₽ · Продукты · Тинькофф · сегодня» с кнопками
  `✓ Записать`, `Категория…`, `Кошелёк…`, «★ Сделать кошелёк основным», `✕`.
  Черновики — в памяти 15 минут.
- Запись — `POST /sync/push` в wallet-api токеном бота (scope
  `expense:write`: только новые операции). Перед записью справочник
  сверяется свежим (удаление счёта на телефоне мягкое — без сверки трата ушла
  бы на удалённую категорию). `batch_id` и uuid операции — UUIDv5 от бота,
  `tg_user_id` и черновика: повторный клик и повтор после таймаута не
  создают дубль. Сервер не ответил — «✓ Проверить ещё раз» тем же батчем,
  правка черновика до выяснения закрыта. Трата уходит на телефоны обычным
  `/sync/pull`, устройство — Telegram.
- Справочник кошельков и категорий (`GET /api/reports/accounts`) кешируется
  на пользователя на 60 с; `/wallets` и `/categories` читают его свежим.
- **Трата свободным текстом через `wallet-ai`** (`WALLET_AI_URL`; пусто — ИИ
  выключен): «кофе 200», «вчера такси 450 с тинькоффа». Бот создаёт задание
  `parse_expense` (`POST /api/ai/jobs`, ключ `tg:<tg_user_id>:<update_id>`),
  сразу отвечает «Разбираю…» и правит это сообщение в обычную карточку с
  пометкой «Понял из текста — проверьте перед записью»; запись — та же.
  ИИ зовётся только там, где строгий формат сдался (таблица 9c в
  `org/docs/TODO_telegram_bot.md`):
  - текст не с суммы, но с цифрой;
  - категория не нашлась («350 потратил на кофе»);
  - сумма не разобралась («1,200.50»).

  Сообщения без цифры и длиннее 300 байт идут без ИИ. Если ИИ не помог
  (выключен, занят, не нашёл трату, ответ не прошёл проверки, нет итога за
  90 с), бот показывает ровно то, что ответил бы без ИИ.

  Итог задания бот забирает в том же потоке поллинга: пока задания есть,
  `getUpdates` ждёт 1 с, а после него идёт короткий `GET /api/ai/jobs/{id}`.
  HTTP-клиент Telegram, база и черновики однопоточны. Ожидания хранятся в
  памяти: рестарт бота во время разбора оставит «Разбираю…» без итога —
  пользователь напишет ещё раз.

## Устройство

| Файл | Что делает |
| --- | --- |
| `src/main.cpp` | конфиг, служебный HTTP в фоне, цикл поллинга в главном потоке |
| `src/bot.{hpp,cpp}` | разбор команд, ответы, `run_poll_loop` |
| `src/telegram_client.{hpp,cpp}` | Bot API: `getUpdates`, `sendMessage`/`editMessageText` с inline-клавиатурой, `answerCallbackQuery`; интерфейс ради фейков в тестах |
| `src/auth_client.{hpp,cpp}` | google-auth: обмен кода связывания, токен бота, `/unlink` |
| `src/reports_client.{hpp,cpp}` | wallet-report: GET с Bearer-токеном бота |
| `src/wallet_client.{hpp,cpp}` | wallet-api: `POST /sync/push` одной новой операции (scope `expense:write`), разбор ответа |
| `src/sessions.{hpp,cpp}` | кеш токенов бота по `tg_user_id` |
| `src/text.{hpp,cpp}` | сопоставление имён, формат сумм и дат |
| `src/parser/strict.{hpp,cpp}` | строгий формат траты (шаг 6) |
| `src/resolver.{hpp,cpp}` | трата → кошелёк и категория или вопрос (§3.2 плана) |
| `src/drafts.{hpp,cpp}` | черновики трат в памяти: TTL 15 мин, ≤ 20 на пользователя |
| `src/callback.{hpp,cpp}` | `callback_data` кнопок: `<draft_id>:<действие>[:<число>]` |
| `src/card.{hpp,cpp}` | текст и клавиатура черновика: вопрос о категории, о кошельке, карточка |
| `src/link_repo.{hpp,cpp}` | своя база: настройки `tg_link`, `tg_update_offset` |
| `src/util/tz.{hpp,cpp}` | часовые пояса через системную tzdata |
| `src/db.*`, `src/util/{log,metrics,dotenv,uuid,hex}.*`, `src/docs.*` | копии из wallet-report (общей библиотеки пока нет) |

Служебные ручки на `:8086`: `/healthz`, `/version`, `/metrics`, `/openapi.yaml`, `/docs`.
Через wallet-gateway бот не публикуется.

## Запуск в compose

1. Роль, база и схема: добавьте `TELEGRAM_POSTGRES_*` в `infra/db/.env` (см.
   `.env.example` там же) и выполните `bash infra/db/migrate.sh telegram up`
   (или Jenkins `db-migrate`). На существующем volume `db-data` образ
   postgres `init-db.sh` сам уже не запустит — `migrate.sh` видит, что базы
   нет, и прогоняет его сам (идемпотентно), потом накатывает миграции.
   Нужна и миграция `auth` (таблица `telegram_links`): `migrate.sh auth up`
   или `db-migrate` с `all`.
2. google-auth: в `google-auth/.env` задать `TELEGRAM_BOT_USERNAME` и
   `TELEGRAM_SERVICE_SECRET`, перезапустить google-auth.
3. `cp wallet-telegram/.env.example wallet-telegram/.env` и заполнить: токен
   бота и тот же `TELEGRAM_SERVICE_SECRET`.
4. `docker compose --env-file infra/db/.env up -d --build wallet-telegram`

Проверка без веба: получить код можно запросом с пользовательским токеном,
`POST /auth/telegram/link-code` → открыть `deep_link` на телефоне.

## Тесты

Юнит-тесты без сети и базы (фейки Telegram/google-auth/хранилища):

```bash
docker build --target test -f wallet-telegram/Containerfile wallet-telegram
```

Сборка образа падает, если хоть один тест не прошёл.

Интеграционные — против живого dev-стека (google-auth, wallet-report,
wallet-api), с фейковым Telegram: `bash wallet-telegram/tests_integration/run_local.sh`,
подробности — [tests_integration/README.md](tests_integration/README.md).

## Метрики

`GET /metrics` на `:8086`, Prometheus собирает его как job `wallet-telegram`;
панели — раздел «Telegram-бот» дашборда Overview.

| Метрика | Что |
| --- | --- |
| `tg_updates_total{type}` | апдейты по типу обработки (команда, `expense`, `callback`, ...) |
| `tg_expense_total{result}` | сообщение-трата: `resolved`, `ambiguous`, `rejected`, `parse_failed` |
| `tg_parse_fail_total{reason}` | почему не разобрано |
| `tg_expense_save_total{result}` | исход «✓ Записать»: `saved`, `unavailable`, `lost`, `reader`, `rejected`, ... |
| `tg_callbacks_total{result}` | нажатия кнопок: `ok`, `stale`, `foreign`, `bad_data`, ... |
| `tg_wallet_push_duration_seconds{status}` | длительность `POST /sync/push` |
| `tg_telegram_api_duration_seconds{method}` | длительность вызовов Bot API, кроме `getUpdates` |
| `tg_api_errors_total{method}`, `tg_wallet_api_errors_total{kind}`, `tg_reports_errors_total{kind}` | ошибки внешних вызовов |
| `tg_last_poll_ok_timestamp_seconds` | время последнего успешного `getUpdates` |
| `tg_ai_job_total{result}` | создание задания wallet-ai: `created`, `unavailable`, `busy`, `error`, ... |
| `tg_ai_outcome_total{outcome}` | итог: `draft`, `not_expense`, `rejected`, `failed`, `expired`, `gave_up`, ... |
| `tg_ai_wait_seconds{outcome}` | от «Разбираю…» до итога |
| `tg_expense_source_total{source}` | черновики по источнику: `strict`, `llm` |
| `tg_ai_errors_total{kind}` | ошибки запросов к wallet-ai |

Суммы, имена и тексты в метрики не попадают — лейблы только из литералов кода.

## Логи и приватность

В логах — `tg_user_id`, `update_id`, `trace_id`, события (`tg_linked`,
`tg_link_rejected`, `tg_update_failed`, ...). Тексты сообщений, суммы, коды
связывания и токены в логи не пишутся. Токен бота входит в URL запросов к Bot
API, поэтому URL запросов к Telegram не логируются.

## Версия

`GET /version`, строка лога при старте и `build_info` в `GET /metrics` —
общая схема C++-сервисов проекта, см. [README google-auth, «Версия»](https://github.com/WalletTransactions/google-auth#версия).
Там же — `build` (тег образа из `--build-arg APP_BUILD`) и `started_at`
(время запуска) в `GET /version`. Гейтвей опрашивает `/version` бота для
сводки `GET /api/versions`, если ему задан `WALLET_TELEGRAM_URL`; сам бот
через гейтвей по-прежнему не публикуется.

Релиз — git-тегами: `vX.Y.Z-rc.N` публикует образ в ghcr и выкатывает его
в test, `vX.Y.Z` на том же коммите — тот же образ в prod; `X.Y.Z` должен
совпадать с `VERSION`. Подробно — [README google-auth, «Версия»](https://github.com/WalletTransactions/google-auth#версия), «Релиз».
