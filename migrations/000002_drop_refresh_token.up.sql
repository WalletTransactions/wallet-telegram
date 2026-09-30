-- Бот больше не хранит refresh-токены пользователей
-- (servers/TODO_telegram_tokens.md, шаг 6): связка tg_user_id → user_id
-- переехала в google-auth (telegram_links), токен бот берёт на каждую
-- команду по tg_user_id. Колонка с зашифрованным refresh больше не нужна —
-- и секретов в базе бота не остаётся вовсе.
--
-- Строки tg_link не удаляются: это настройки (пояс, кошелёк по умолчанию).
-- Связок, созданных до шага 4 (миграция auth/000004), в google-auth нет —
-- на первой же команде бот получит 404 и попросит подключиться заново.
ALTER TABLE tg_link DROP COLUMN refresh_token_enc;

COMMENT ON TABLE tg_link IS
    'Настройки Telegram-пользователя (чат, пояс, кошелёк по умолчанию). Сама связка с пользователем — в google-auth (telegram_links).';
