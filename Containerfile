# syntax=docker/dockerfile:1
#
# Multi-stage build wallet-telegram. Стадии:
#   build   — сервис без тестов (то, что уходит в runtime);
#   test    — юнит-тесты, падают сборкой образа, если тест не прошёл:
#             docker build --target test -f wallet-telegram/Containerfile wallet-telegram
#   runtime — итоговый образ (по умолчанию, его собирает compose).

FROM debian:bookworm-slim AS deps

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libssl-dev \
        libpq-dev \
        pkg-config \
        tzdata \
    && rm -rf /var/lib/apt/lists/*
# tzdata — юнит-тесты /tz проверяют настоящие файлы поясов (см. runtime ниже).

WORKDIR /src
COPY CMakeLists.txt ./
COPY VERSION ./
COPY openapi.yaml ./
COPY src/ src/
COPY third_party/ third_party/

FROM deps AS build
# GIT_COMMIT — см. Containerfile wallet-report, тот же приём.
ARG GIT_COMMIT=unknown
ARG BUILD_JOBS=3
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DWALLET_TELEGRAM_BUILD_TESTS=OFF -DGIT_COMMIT="${GIT_COMMIT}" \
    && cmake --build build --config Release -j"${BUILD_JOBS}"

FROM deps AS test
COPY tests/ tests/
ARG BUILD_JOBS=3
RUN cmake -B build-tests -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build-tests --target wallet_telegram_tests -j"${BUILD_JOBS}" \
    && ./build-tests/tests/wallet_telegram_tests

FROM debian:bookworm-slim AS runtime

# ca-certificates — обязательно: без системных CA httplib не проверит
# сертификат api.telegram.org. tzdata — часовые пояса пользователей (/tz,
# «сегодня» для отчётов, см. src/util/tz.hpp). curl — только для healthcheck
# в compose.yaml.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        tzdata \
        libssl3 \
        libpq5 \
        curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /app --shell /usr/sbin/nologin appuser

WORKDIR /app
COPY --from=build /src/build/wallet_telegram /app/wallet_telegram
COPY openapi.yaml /app/openapi.yaml
COPY third_party/swagger-ui/ /app/vendor/swagger-ui/

USER appuser
EXPOSE 8086

ENTRYPOINT ["/app/wallet_telegram"]
