# Интеграционные тесты wallet-telegram

Шаг 8 [плана бота](https://github.com/WalletTransactions/.github-private/blob/main/docs/TODO_telegram_bot.md): бот с **настоящими**
HTTP-клиентами против живого dev-стека — google-auth, wallet-report,
wallet-api и их Postgres/Redis. Фейковые только Telegram (апдейты подаются
прямо в `Bot::handle`) и своя база бота (`LinkStore` в памяти — её схему
проверяют юнит-тесты и `migrate.sh`).

## Сценарий (`test_flow.cpp`)

1. «Телефон» (токен пользователя с `device_id`) загружает иконку и одним батчем
   заводит валюту, кошелёк «Наличные» (₽) и категорию «Продукты».
2. `/start <код>` — обмен кода на связку в google-auth.
3. `/categories`, `/wallets` — чтение из wallet-report токеном бота.
4. `350,5 продукты // …` — карточка, `✓ Записать` — запись в wallet-api,
   второй клик — «Уже записано».
5. `/sync/pull` телефона видит ровно одну операцию: из кошелька в
   категорию, сумма и комментарий из сообщения, `updated_by` — устройство
   бота.
6. Границы токена бота (`expense:write`): правка той же операции —
   `403 operation_exists`, запись счёта — `403 only_operation`, `/sync/pull`
   — `403`; операция не изменилась.
7. `/unlink` — связка удалена, google-auth больше не выдаёт токен (404).
8. Уборка: `DELETE /api/account` телефоном (леджер целиком).

## Как запустить

```bash
# стек со свежими wallet-api и google-auth
docker compose --env-file infra/db/.env up -d --build wallet-api google-auth
bash wallet-telegram/tests_integration/run_local.sh
```

Нужны `infra/db/.env`, `wallet-telegram/.env` (`TELEGRAM_SERVICE_SECRET` —
тот же, что в `google-auth/.env`) и `google-auth/jwt_signing_key.pem`.

`run_local.sh`:
- заводит пользователя **прямо в базе google-auth** и кладёт код связывания в
  Redis (`tg_link_code:<base64url(sha256(код))>`) — google-auth пускает на
  `POST /auth/telegram/link-code` только свои токены, а вход через Google в
  тесте невозможен;
- выпускает токен «телефона» `mint_loadtest_token` (volume
  `google_auth_loadtest_build`, общий с `wallet-api/loadtest/mint_tokens.sh`);
- собирает образ по `Containerfile` рядом и запускает его в сети стека;
- в конце (`trap`, и при падении) удаляет пользователя из google-auth —
  связка `telegram_links` уходит каскадом — и код из Redis.

**Это dev-стек, не изолированный контур:** тест пишет в те же базы, что и
ваша ручная работа, — каждый прогон заводит своего пользователя со случайным
id и убирает его за собой. Сбой посреди прогона может оставить леджер в
wallet-api (если не дошло до `DELETE /api/account`) — он ни с кем не связан.
Секреты из `.env` передаются в контейнер по имени переменной и в вывод не
печатаются.
