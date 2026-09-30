# Тот же самодельный харнесс, что у юнит-тестов (../tests/test_framework.hpp,
# ../tests/test_main.cpp) — копии не заводим.
add_executable(wallet_telegram_integration_tests
    ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
    test_flow.cpp
)
target_include_directories(wallet_telegram_integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(wallet_telegram_integration_tests PRIVATE wallet_telegram_core)
