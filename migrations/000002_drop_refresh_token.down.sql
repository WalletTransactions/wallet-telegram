-- Токены при откате не восстановить: колонка возвращается пустой (bytea
-- без NOT NULL), бот старой версии попросит переподключиться.
ALTER TABLE tg_link ADD COLUMN refresh_token_enc bytea;

COMMENT ON TABLE tg_link IS 'Связка Telegram-пользователя с пользователем кошелька: device-сессия бота platform=telegram.';
