cmake_minimum_required(VERSION 3.16)
project(wallet_telegram CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Общий код сервисов (util/: jwt, jwks, crypto, log, metrics, ...) — копия
# репозитория wallet-common, обновляется его scripts/vendor.sh; руками не править.
set(WALLET_COMMON_DIR ${CMAKE_SOURCE_DIR}/third_party/wallet-common)

# OpenSSL — HTTPS до api.telegram.org (httplib с CPPHTTPLIB_OPENSSL_SUPPORT)
# и SHA-1 для UUIDv5 (src/util/uuid.cpp).
# PostgreSQL — своя база бота (связки чатов, offset поллинга).
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)

# Версия — тот же приём, что у остальных сервисов (см. CMakeLists.txt
# wallet-report, "Версия приложения"): номер из VERSION, хэш — из
# -DGIT_COMMIT (Containerfile) или из git при нативной сборке.
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION APP_VERSION_LINES LIMIT_COUNT 1)
list(GET APP_VERSION_LINES 0 APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)

# Сборка — тег образа (`0.1.1-rc.2`), отдельно от VERSION: по VERSION сверяет
# выкат smoke в wallet-infra deploy.sh. Release передаёт тег через
# --build-arg APP_BUILD (Containerfile → -DAPP_BUILD); сборка без него
# помечается `-dev`, чтобы её не спутать с релизной (GET /version, поле
# build; wallet-gateway openspec/changes/service-versions, design D1/D2).
if(NOT DEFINED APP_BUILD OR "${APP_BUILD}" STREQUAL "")
    set(APP_BUILD "${APP_VERSION}-dev")
endif()

if(NOT DEFINED GIT_COMMIT OR "${GIT_COMMIT}" STREQUAL "")
    find_program(GIT_EXECUTABLE git)
    set(GIT_COMMIT "unknown")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty --abbrev=8
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE GIT_DESCRIBE_RESULT
            ERROR_QUIET
        )
        if(NOT GIT_DESCRIBE_RESULT EQUAL 0 OR "${GIT_COMMIT}" STREQUAL "")
            set(GIT_COMMIT "unknown")
        endif()
    endif()
endif()
message(STATUS "wallet_telegram version: ${APP_VERSION}, build ${APP_BUILD} (${GIT_COMMIT})")

configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

# Всё, кроме main.cpp и docs.cpp, — библиотека: её же линкуют юнит-тесты.
add_library(wallet_telegram_core STATIC
    src/bot.cpp
    src/callback.cpp
    src/card.cpp
    src/drafts.cpp
    src/telegram_client.cpp
    src/ai_client.cpp
    src/auth_client.cpp
    src/link_repo.cpp
    ${WALLET_COMMON_DIR}/expense/strict.cpp
    ${WALLET_COMMON_DIR}/clients/reports_client.cpp
    ${WALLET_COMMON_DIR}/expense/resolver.cpp
    src/sessions.cpp
    ${WALLET_COMMON_DIR}/expense/text.cpp
    src/db.cpp
    ${WALLET_COMMON_DIR}/util/dotenv.cpp
    ${WALLET_COMMON_DIR}/util/hex.cpp
    ${WALLET_COMMON_DIR}/util/log.cpp
    ${WALLET_COMMON_DIR}/util/metrics.cpp
    src/util/tz.cpp
    ${WALLET_COMMON_DIR}/util/uuid.cpp
    src/wallet_client.cpp
)
target_include_directories(wallet_telegram_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${WALLET_COMMON_DIR}
    ${CMAKE_BINARY_DIR}/generated
)
target_compile_definitions(wallet_telegram_core PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
target_compile_options(wallet_telegram_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(wallet_telegram_core PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    PostgreSQL::PostgreSQL
)
if(WIN32)
    target_link_libraries(wallet_telegram_core PUBLIC ws2_32 crypt32)
endif()

add_executable(wallet_telegram
    src/main.cpp
    src/docs.cpp
)
target_link_libraries(wallet_telegram PRIVATE wallet_telegram_core)

add_custom_command(TARGET wallet_telegram POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/openapi.yaml
            $<TARGET_FILE_DIR:wallet_telegram>/openapi.yaml
    COMMENT "Копирую openapi.yaml рядом с wallet_telegram (GET /openapi.yaml, /docs)"
)
add_custom_command(TARGET wallet_telegram POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/third_party/swagger-ui
            $<TARGET_FILE_DIR:wallet_telegram>/vendor/swagger-ui
    COMMENT "Копирую вендоренный Swagger UI рядом с wallet_telegram (GET /docs)"
)

# Юнит-тесты — без сети и базы (фейки TelegramApi/AuthApi/LinkStore, см.
# tests/test_bot.cpp). В образе запускаются стадией `test` Containerfile:
#   docker build --target test -f wallet-telegram/Containerfile wallet-telegram
option(WALLET_TELEGRAM_BUILD_TESTS "Собирать юнит-тесты wallet_telegram_tests" ON)
if(WALLET_TELEGRAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Интеграционные тесты — против живого dev-стека (google-auth, wallet-api,
# wallet-report), по умолчанию не собираются. Запуск — только через
# tests_integration/run_local.sh: он готовит пользователя, код связывания и
# токен «телефона».
option(WALLET_TELEGRAM_BUILD_INTEGRATION_TESTS "Собирать wallet_telegram_integration_tests" OFF)
if(WALLET_TELEGRAM_BUILD_INTEGRATION_TESTS)
    add_subdirectory(tests_integration)
endif()
