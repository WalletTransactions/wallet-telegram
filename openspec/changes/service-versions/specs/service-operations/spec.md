# Spec Delta

## MODIFIED Requirements

### Requirement: Только служебные HTTP-ручки
Сервис SHALL отдавать по HTTP только служебные ручки без авторизации:
`/healthz`, `/version`, `/metrics`, `/openapi.yaml`, `/docs` и статику
Swagger UI под `/vendor/swagger-ui`. Публичного API у бота нет, через
wallet-gateway он не публикуется. Адрес по умолчанию `127.0.0.1:8086`
(`HOST`, `PORT`).

#### Scenario: Версия билда
- **WHEN** вызывается `GET /version`
- **THEN** ответ — JSON с `service: wallet-telegram`, `version` (содержимое файла `VERSION`, без суффикса `-rc.N`), `build` (тег образа из `APP_BUILD`, например `0.1.0-rc.2`; без него `<VERSION>-dev`), `git_commit` (коммит сборки из `GIT_COMMIT`, `unknown`, если не задан) и `started_at` (время запуска процесса, ISO-8601 UTC)

#### Scenario: Время запуска в метриках
- **WHEN** вызывается `GET /metrics`
- **THEN** в ответе есть `build_info` с меткой `build` и `process_start_time_seconds`, совпадающий с `started_at` из `/version` с точностью до секунды
