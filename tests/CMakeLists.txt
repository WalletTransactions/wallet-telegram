add_executable(wallet_telegram_tests
    test_main.cpp
    test_parsing.cpp
    test_text.cpp
    test_strict.cpp
    test_resolver.cpp
    test_callback.cpp
    test_drafts.cpp
    test_wallet_client.cpp
    test_metrics.cpp
    test_bot.cpp
    test_ai_client.cpp
    test_chat_routing.cpp
)
target_include_directories(wallet_telegram_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wallet_telegram_tests PRIVATE wallet_telegram_core)

add_test(NAME wallet_telegram_unit_tests COMMAND wallet_telegram_tests)
