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
