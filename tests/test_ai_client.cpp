// Разбор ответов wallet-ai (ai_client.hpp) — без сети.
#include "ai_client.hpp"

#include "test_framework.hpp"

TEST(ai_create_response) {
    auto ok = ai::parse_create_response(202, R"({"job_id":"5217ad6d-301b-4dff-8e55-31e560436a05","status":"queued"})");
    CHECK(ok.status == ai::CreateResult::Status::Created);
    CHECK_EQ(ok.job_id, std::string("5217ad6d-301b-4dff-8e55-31e560436a05"));

    CHECK(ai::parse_create_response(503, R"({"error":"unavailable"})").status == ai::CreateResult::Status::Unavailable);
    CHECK(ai::parse_create_response(429, R"({"error":"busy"})").status == ai::CreateResult::Status::Busy);
    CHECK(ai::parse_create_response(401, "").status == ai::CreateResult::Status::Unauthorized);
    auto bad = ai::parse_create_response(400, R"({"error":"input.now обязателен"})");
    CHECK(bad.status == ai::CreateResult::Status::Error);
    CHECK_EQ(bad.error, std::string("HTTP 400: input.now обязателен"));
    CHECK(ai::parse_create_response(202, "not json").status == ai::CreateResult::Status::Error);
}

TEST(ai_job_response) {
    CHECK(ai::parse_job_response(200, R"({"status":"queued"})").status == ai::JobResult::Status::Pending);
    CHECK(ai::parse_job_response(200, R"({"status":"running"})").status == ai::JobResult::Status::Pending);

    auto done = ai::parse_job_response(200, R"({"status":"done","result":{"outcome":"draft","expense":{"amount":"450"}}})");
    REQUIRE(done.status == ai::JobResult::Status::Done);
    CHECK_EQ(done.result["expense"]["amount"].get<std::string>(), std::string("450"));

    auto failed = ai::parse_job_response(200, R"({"status":"failed","failure":"timeout"})");
    CHECK(failed.status == ai::JobResult::Status::Failed);
    CHECK_EQ(failed.failure, std::string("timeout"));

    CHECK(ai::parse_job_response(200, R"({"status":"cancelled"})").status == ai::JobResult::Status::Cancelled);
    CHECK(ai::parse_job_response(404, "").status == ai::JobResult::Status::NotFound);
    CHECK(ai::parse_job_response(401, "").status == ai::JobResult::Status::Unauthorized);
    CHECK(ai::parse_job_response(500, "").status == ai::JobResult::Status::Error);
    CHECK(ai::parse_job_response(200, "garbage").status == ai::JobResult::Status::Error);
    CHECK(ai::parse_job_response(200, R"({"status":"done"})").status == ai::JobResult::Status::Error); // без result
}

TEST(ai_create_chat_response) {
    // 503 provider_unavailable (chat) отличается от «ИИ выключен».
    auto prov = ai::parse_create_response(503, R"({"error":"provider_unavailable","provider":"claude"})");
    CHECK(prov.status == ai::CreateResult::Status::ProviderUnavailable);
    CHECK(ai::parse_create_response(503, R"({"error":"unavailable"})").status == ai::CreateResult::Status::Unavailable);
    CHECK(ai::parse_create_response(503, "").status == ai::CreateResult::Status::Unavailable);
    CHECK(ai::parse_create_response(429, R"({"error":"busy"})").status == ai::CreateResult::Status::Busy);
    CHECK(ai::parse_create_response(400, R"({"error":"bad_request"})").status == ai::CreateResult::Status::Error);
    // Чат: failed с кодами chat — те же поля, что у parse_expense.
    auto failed = ai::parse_job_response(200, R"({"status":"failed","failure":"tools_unavailable"})");
    CHECK(failed.status == ai::JobResult::Status::Failed);
    CHECK_EQ(failed.failure, std::string("tools_unavailable"));
    auto done = ai::parse_job_response(
        200, R"({"status":"done","result":{"text":"2899.00 EUR","provider":"lmstudio","conversation_id":"tg-1-2"}})");
    REQUIRE(done.status == ai::JobResult::Status::Done);
    CHECK_EQ(done.result["provider"].get<std::string>(), std::string("lmstudio"));
}

TEST(ai_settings_response) {
    auto ok = ai::parse_settings_response(
        200, R"({"provider":"lmstudio","providers":[{"id":"lmstudio","available":true,"title":"LM Studio"}]})");
    REQUIRE(ok.status == ai::SettingsResult::Status::Ok);
    CHECK_EQ(ok.settings["provider"].get<std::string>(), std::string("lmstudio"));

    CHECK(ai::parse_settings_response(400, R"({"error":"provider_unavailable"})").status ==
          ai::SettingsResult::Status::ProviderUnavailable);
    CHECK(ai::parse_settings_response(409, R"({"error":"consent_required"})").status ==
          ai::SettingsResult::Status::ConsentRequired);
    CHECK(ai::parse_settings_response(401, "").status == ai::SettingsResult::Status::Unauthorized);
    CHECK(ai::parse_settings_response(503, R"({"error":"settings_unavailable"})").status ==
          ai::SettingsResult::Status::Unavailable);
    // 400 bad_request, битый 200, 200 без списка — как недоступен.
    CHECK(ai::parse_settings_response(400, R"({"error":"bad_request"})").status ==
          ai::SettingsResult::Status::Unavailable);
    CHECK(ai::parse_settings_response(200, "garbage").status == ai::SettingsResult::Status::Unavailable);
    CHECK(ai::parse_settings_response(200, R"({"provider":"lmstudio"})").status ==
          ai::SettingsResult::Status::Unavailable);
}
