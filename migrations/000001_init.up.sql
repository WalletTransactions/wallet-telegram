-- wallet_telegram — собственная база Telegram-бота (servers/TODO_telegram_bot.md,
-- §1.4). Не часть домена кошелька: здесь нет ни одной операции или счёта,
-- только связки «Telegram-чат ↔ пользователь кошелька» и offset поллинга.
-- Поэтому, как и perf, не зеркалится в schema/ (тот каталог — про леджер,
-- общий с Android-клиентом).
--
-- Права: роль бота (TELEGRAM_POSTGRES_USER) получает только DML через
-- ALTER DEFAULT PRIVILEGES в infra/db/init-db.sh; DDL — мигратор.

-- Одна строка на Telegram-пользователя. tg_user_id, а не chat_id, — ключ:
-- бот работает только в личных чатах, где chat_id == user id, но смысл
-- связки — «этот человек в Telegram», а не «этот чат».
CREATE TABLE tg_link (
    tg_user_id          bigint      PRIMARY KEY CHECK (tg_user_id > 0),
    chat_id             bigint      NOT NULL,
    -- Идентификаторы из google-auth/wallet-api. Внешних ключей нет и быть не
    -- может — другие базы.
    user_id             uuid        NOT NULL,
    device_id           uuid        NOT NULL,
    -- secretbox (AES-256-GCM): nonce(12) | ciphertext | tag(16). Ключ —
    -- TELEGRAM_TOKEN_ENC_KEY из .env бота, в базе его нет.
    refresh_token_enc   bytea       NOT NULL,
    tz                  text        NOT NULL DEFAULT 'UTC',
    default_wallet_uuid uuid,
    created_at          timestamptz NOT NULL DEFAULT now(),
    last_used_at        timestamptz NOT NULL DEFAULT now()
);

-- Отзыв и диагностика «какие чаты у этого пользователя».
CREATE INDEX tg_link_user_id_idx ON tg_link (user_id);

-- Ровно одна строка (id = 1): update_id первого необработанного апдейта.
-- После рестарта бот продолжает с него и не переобрабатывает старые апдейты
-- (§1.3).
CREATE TABLE tg_update_offset (
    id             smallint    PRIMARY KEY CHECK (id = 1),
    next_update_id bigint      NOT NULL,
    updated_at     timestamptz NOT NULL DEFAULT now()
);

COMMENT ON TABLE tg_link IS 'Связка Telegram-пользователя с пользователем кошелька: device-сессия бота platform=telegram.';
COMMENT ON COLUMN tg_link.refresh_token_enc IS 'Refresh-токен device-сессии бота, зашифрован AES-256-GCM ключом из .env бота.';
COMMENT ON COLUMN tg_link.tz IS 'Часовой пояс пользователя (IANA) — для дат трат, у operation.date нет TZ.';
COMMENT ON TABLE tg_update_offset IS 'Одна строка: update_id, с которого продолжать getUpdates после рестарта.';
