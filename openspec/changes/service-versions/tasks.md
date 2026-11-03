# Tasks

## 1. Сборка и `/version`

- [ ] 1.1 `bash ../wallet-common/scripts/vendor.sh .` (после задачи 1.1 change wallet-gateway `service-versions`); `src/main.cpp` — новая `set_build_info(service, version, build, commit)` и `build` в строке лога при старте; `tests/test_metrics.cpp` — проверки метки `build` и `process_start_time_seconds`, `tests/test_bot.cpp` — новая сигнатура. Проверка: `ctest` в Docker проходит
- [ ] 1.2 `CMakeLists.txt` (`APP_BUILD`, по умолчанию `${APP_VERSION}-dev`), `src/version.hpp.in` (`kBuild`), `Containerfile` (`ARG APP_BUILD`, `-DAPP_BUILD`). Проверка: образ без аргументов → `curl 127.0.0.1:8086/version` в контейнере содержит `"build":"<VERSION>-dev"`
- [ ] 1.3 `src/docs.cpp`: `build` и `started_at` в `/version`; `openapi.yaml` (схема `/version`) и README. Проверка: два `curl /version` подряд — одинаковый `started_at`
- [ ] 1.4 `.github/workflows/release.yml`: `APP_BUILD=${{ needs.meta.outputs.version }}-rc.${{ needs.meta.outputs.rc }}` в `build-args` rc-image; rc-тег. Проверка: выкат в test прошёл smoke, элемент wallet-telegram в `GET /api/versions` гейтвея (при заданном `WALLET_TELEGRAM_URL`) с `build` = тег
