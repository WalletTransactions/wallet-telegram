#!/bin/bash
# Прогоняет wallet_telegram_integration_tests против ЖИВОГО dev-стека
# (wallet-infra/compose.yaml: db, redis, google-auth, wallet-api, wallet-report) —
# см. README.md рядом.
#
# Что делает:
#   1. заводит тестового пользователя прямо в базе google-auth (users) и
#      кладёт код связывания в Redis — google-auth пускает на
#      POST /auth/telegram/link-code только токены, выданные им самим, а
#      вход через Google в тесте невозможен;
#   2. выпускает «телефонный» токен этого пользователя через
#      mint_loadtest_token (тот же инструмент и docker volume, что у
#      wallet-api/loadtest/mint_tokens.sh) — wallet-api и wallet-report
#      проверяют токены только по JWKS;
#   3. собирает тестовый образ и запускает его в сети стека;
#   4. в любом случае (trap) удаляет пользователя из google-auth (связка
#      telegram_links уходит каскадом) и код из Redis; леджер в wallet-api
#      удаляет сам тест (DELETE /api/account).
#
# Требования: стек поднят (`docker compose --env-file infra/db/.env up -d`),
# wallet-api и google-auth — свежие образы (expense:write на /sync/push),
# есть infra/db/.env, wallet-telegram/.env (TELEGRAM_SERVICE_SECRET — тот же,
# что в google-auth/.env) и google-auth/jwt_signing_key.pem.
#
# Секреты из .env попадают только в переменные этого скрипта и в окружение
# тестового контейнера — в вывод не печатаются.
set -euo pipefail
export MSYS_NO_PATHCONV=1

ENGINE=docker
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
GA_DIR="$WORKSPACE_DIR/google-auth"
GA_BUILD_VOL="google_auth_loadtest_build"
TEST_IMAGE=wallet-telegram-it-tests

DB_ENV="$WORKSPACE_DIR/wallet-infra/infra/db/.env"
BOT_ENV="$WORKSPACE_DIR/wallet-telegram/.env"
for f in "$DB_ENV" "$BOT_ENV" "$GA_DIR/jwt_signing_key.pem"; do
    [[ -f "$f" ]] || { echo "Не найден $f — см. README.md рядом." >&2; exit 1; }
done
set -a
# shellcheck disable=SC1090
source "$DB_ENV"
set +a
# Из .env бота — только секрет сервиса, остальное не нужно.
TELEGRAM_SERVICE_SECRET="$(grep -E '^TELEGRAM_SERVICE_SECRET=' "$BOT_ENV" | tail -n1 | cut -d= -f2-)"
[[ -n "$TELEGRAM_SERVICE_SECRET" ]] || { echo "В $BOT_ENV пуст TELEGRAM_SERVICE_SECRET." >&2; exit 1; }

container_of() {
    "$ENGINE" ps --filter "label=com.docker.compose.service=$1" --format '{{.Names}}' | head -n1
}
db_container="$(container_of db)"
redis_container="$(container_of redis)"
for svc in db redis google-auth wallet-api wallet-report; do
    [[ -n "$(container_of "$svc")" ]] || { echo "Не запущен сервис $svc — поднимите стек." >&2; exit 1; }
done
network="$("$ENGINE" inspect "$db_container" --format '{{range $k, $v := .NetworkSettings.Networks}}{{$k}}{{end}}')"

gen_uuid() { python3 -c "import uuid; print(uuid.uuid4())"; }
b64url() { base64 | tr '+/' '-_' | tr -d '=\n'; }

IT_USER_ID="$(gen_uuid)"
IT_EMAIL="it-telegram-${IT_USER_ID:0:8}@example.com"
IT_DEVICE_ID="$(gen_uuid)"
IT_TG_USER_ID="$((9000000000 + RANDOM * 32768 + RANDOM))"
IT_LINK_CODE="$(openssl rand 24 | b64url)"
code_key="tg_link_code:$(printf '%s' "$IT_LINK_CODE" | openssl dgst -sha256 -binary | b64url)"

auth_psql() {
    "$ENGINE" exec -i -e PGPASSWORD="$AUTH_POSTGRES_PASSWORD" "$db_container" \
        psql -v ON_ERROR_STOP=1 -q -U "$AUTH_POSTGRES_USER" -d "$AUTH_POSTGRES_DB" "$@"
}

cleanup() {
    echo "==> Уборка: пользователь $IT_USER_ID в google-auth, код связывания"
    auth_psql -c "DELETE FROM users WHERE id = '$IT_USER_ID'" >/dev/null || true
    "$ENGINE" exec "$redis_container" redis-cli DEL "$code_key" >/dev/null || true
}
trap cleanup EXIT

echo "==> Пользователь $IT_USER_ID в google-auth и код связывания в Redis"
auth_psql -c "INSERT INTO users (id, google_sub, email, email_verified, name)
              VALUES ('$IT_USER_ID', 'it-telegram-$IT_USER_ID', '$IT_EMAIL', true, 'Integration Telegram')"
"$ENGINE" exec "$redis_container" redis-cli SET "$code_key" "$IT_USER_ID" EX 600 >/dev/null

echo "==> Токен «телефона» (mint_loadtest_token)"
if ! "$ENGINE" run --rm -v "$GA_BUILD_VOL:/gabin" debian:bookworm-slim test -x /gabin/mint_loadtest_token 2>/dev/null; then
    echo "    собираю mint_loadtest_token (первый раз — дольше)"
    "$ENGINE" run --rm -v "$GA_DIR:/ga" -v "$GA_BUILD_VOL:/ga/build-loadtest" debian:bookworm-slim bash -c '
        set -e
        apt-get update -qq
        apt-get install -y --no-install-recommends -qq build-essential cmake libssl-dev libpq-dev pkg-config >/dev/null
        cd /ga
        cmake -B build-loadtest -DGOOGLE_AUTH_BUILD_TESTS=OFF -DGOOGLE_AUTH_BUILD_LOADTEST_TOOLS=ON >/dev/null
        cmake --build build-loadtest --target mint_loadtest_token -j"$(nproc)" >/dev/null'
fi
IT_PHONE_TOKEN="$("$ENGINE" run --rm -v "$GA_DIR:/ga:ro" -v "$GA_BUILD_VOL:/ga/build-loadtest" \
    -e SUB="$IT_USER_ID" -e EMAIL="$IT_EMAIL" -e DEV="$IT_DEVICE_ID" debian:bookworm-slim bash -c '
    apt-get update -qq && apt-get install -y --no-install-recommends -qq libssl3 >/dev/null
    /ga/build-loadtest/mint_loadtest_token --pem /ga/jwt_signing_key.pem \
        --sub "$SUB" --email "$EMAIL" --device-id "$DEV" --platform android --ttl-secs 900' 2>/dev/null)"
[[ -n "$IT_PHONE_TOKEN" ]] || { echo "Не удалось выпустить токен телефона." >&2; exit 1; }
# Передаются в контейнер по имени (-e NAME), не значением в командной строке —
# чтобы не светиться в `ps`.
export TELEGRAM_SERVICE_SECRET IT_PHONE_TOKEN

echo "==> Сборка тестового образа"
"$ENGINE" build -q -t "$TEST_IMAGE" -f "$SCRIPT_DIR/Containerfile" "$WORKSPACE_DIR/wallet-telegram" >/dev/null

echo "==> Тесты"
"$ENGINE" run --rm --network "$network" \
    -e AUTH_SERVICE_URL=http://google-auth:8080 \
    -e WALLET_REPORT_URL=http://wallet-report:8084 \
    -e WALLET_API_URL=http://wallet-api:8082 \
    -e TELEGRAM_SERVICE_SECRET \
    -e IT_LINK_CODE="$IT_LINK_CODE" \
    -e IT_USER_ID="$IT_USER_ID" \
    -e IT_TG_USER_ID="$IT_TG_USER_ID" \
    -e IT_PHONE_TOKEN \
    "$TEST_IMAGE"
