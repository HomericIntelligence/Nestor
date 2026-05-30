// test_routes.cpp — HTTP route integration tests.
// Updated to use new register_routes signature with AuthMiddleware and RateLimiter.

#include "projectnestor/auth_middleware.hpp"
#include "projectnestor/nats_client.hpp"
#include "projectnestor/rate_limiter.hpp"
#include "projectnestor/routes.hpp"
#include "projectnestor/store.hpp"

#include <memory>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectnestor::test {

using json = nlohmann::json;

// ── Base fixture: auth disabled (empty token = dev mode) ───────────────────

class RoutesTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  httplib::Server server_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  AuthMiddleware auth_{""};   // auth disabled
  RateLimiter rate_{1000.0};  // very high limit — don't hit it in normal tests
  int port_{0};
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

void RoutesTest::SetUp() {
  register_routes(server_, store_, nats_, auth_, rate_);
  port_ = server_.bind_to_any_port("127.0.0.1");
  thread_ = std::thread([this]() { server_.listen_after_bind(); });
  client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
  client_->set_connection_timeout(5);
  client_->set_read_timeout(5);
}

void RoutesTest::TearDown() {
  server_.stop();
  thread_.join();
}

TEST_F(RoutesTest, HealthEndpointReturnsOk) {
  const auto res = client_->Get("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
}

TEST_F(RoutesTest, StatsEndpointReturnsZeros) {
  const auto res = client_->Get("/v1/research/stats");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_FALSE(body.contains("active"));
  EXPECT_EQ(body["completed"], 0);
  EXPECT_EQ(body["pending"], 0);
}

TEST_F(RoutesTest, PostResearchReturns202) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);
  const auto body = json::parse(res->body);
  EXPECT_FALSE(body["id"].get<std::string>().empty());
  EXPECT_EQ(body["status"], "pending");
}

TEST_F(RoutesTest, PostResearchInvalidJsonReturns400) {
  const auto res = client_->Post("/v1/research", "not json", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Invalid JSON");
}

// Issue #41: empty body should fail validation (missing "idea" field).
TEST_F(RoutesTest, PostResearchEmptyBodyReturns400) {
  const auto res = client_->Post("/v1/research", "{}", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

// Issue #41: missing "idea" field.
TEST_F(RoutesTest, PostResearchMissingIdeaReturns400) {
  const std::string payload = R"({"context":"some context"})";
  const auto res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_TRUE(body["detail"].get<std::string>().find("idea") != std::string::npos);
}

// Issue #41: "idea" field of wrong type (integer instead of string).
TEST_F(RoutesTest, PostResearchWrongIdeaTypeReturns400) {
  const std::string payload = R"({"idea": 42})";
  const auto res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

// Issue #41: "context" field of wrong type.
TEST_F(RoutesTest, PostResearchWrongContextTypeReturns400) {
  const std::string payload = R"({"idea": "valid", "context": 123})";
  const auto res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

// Issue #41: oversized body returns 413 before parsing.
TEST_F(RoutesTest, PostResearchOversizedBodyReturns413) {
  const std::string large_body(65 * 1024 + 1, 'x');  // > 64 KiB
  const auto res = client_->Post("/v1/research", large_body, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 413);
}

TEST_F(RoutesTest, StatsReflectsSubmission) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  client_->Post("/v1/research", payload, "application/json");

  const auto res = client_->Get("/v1/research/stats");
  ASSERT_TRUE(res);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["pending"], 1);
}

// Issue #67: complete with required "summary" field.
TEST_F(RoutesTest, CompleteResearchReturns200) {
  const std::string payload = R"({"idea":"test idea","context":"ctx"})";
  const auto submit_res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(submit_res);
  ASSERT_EQ(submit_res->status, 202);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  // Complete it with required summary.
  const std::string complete_payload = R"({"summary":"Research complete"})";
  const auto complete_res =
      client_->Post("/v1/research/" + id + "/complete", complete_payload, "application/json");
  ASSERT_TRUE(complete_res);
  EXPECT_EQ(complete_res->status, 200);
  const auto body = json::parse(complete_res->body);
  EXPECT_EQ(body["status"], "completed");
  EXPECT_EQ(body["id"], id);
}

// Issue #67: complete with empty body should return 400 (missing summary).
TEST_F(RoutesTest, CompleteResearchEmptyBodyReturns400) {
  const std::string payload = R"({"idea":"test idea","context":"ctx"})";
  const auto submit_res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  const auto complete_res =
      client_->Post("/v1/research/" + id + "/complete", "", "application/json");
  ASSERT_TRUE(complete_res);
  EXPECT_EQ(complete_res->status, 400);
}

TEST_F(RoutesTest, CompleteResearchUnknownIdReturns404) {
  const std::string complete_payload = R"({"summary":"Done"})";
  const auto res =
      client_->Post("/v1/research/nonexistent-id/complete", complete_payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["error"], "not_found");
}

TEST_F(RoutesTest, StatsReflectsCompletion) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto submit_res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  const std::string complete_payload = R"({"summary":"Done"})";
  client_->Post("/v1/research/" + id + "/complete", complete_payload, "application/json");

  const auto res = client_->Get("/v1/research/stats");
  ASSERT_TRUE(res);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["pending"], 0);
  EXPECT_EQ(body["completed"], 1);
}

// Issue #64: GET /v1/research (paginated list).
TEST_F(RoutesTest, GetResearchListReturnsItems) {
  const std::string p1 = R"({"idea":"first idea"})";
  const std::string p2 = R"({"idea":"second idea"})";
  client_->Post("/v1/research", p1, "application/json");
  client_->Post("/v1/research", p2, "application/json");

  const auto res = client_->Get("/v1/research");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("items"));
  EXPECT_GE(body["total"].get<int>(), 2);
  EXPECT_TRUE(body["items"].is_array());
}

// Issue #64: GET /v1/research/:id returns the item.
TEST_F(RoutesTest, GetResearchByIdReturnsItem) {
  const std::string payload = R"({"idea":"get by id test"})";
  const auto submit_res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  const auto res = client_->Get("/v1/research/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["id"].get<std::string>(), id);
  EXPECT_EQ(body["status"], "pending");
}

// Issue #64: GET /v1/research/:id returns 404 for unknown id.
TEST_F(RoutesTest, GetResearchByIdUnknownReturns404) {
  const auto res = client_->Get("/v1/research/nonexistent-id");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// Issue #49: X-Correlation-ID passed in request is echoed in response.
TEST_F(RoutesTest, CorrelationIdEchoedInResponse) {
  const std::string payload = R"({"idea":"correlation test"})";
  httplib::Headers headers = {{"X-Correlation-ID", "test-corr-123"}};
  const auto res = client_->Post("/v1/research", headers, payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->get_header_value("X-Correlation-ID"), "test-corr-123");
}

// Issue #49: X-Correlation-ID is generated if absent.
TEST_F(RoutesTest, CorrelationIdGeneratedIfAbsent) {
  const std::string payload = R"({"idea":"auto corr test"})";
  const auto res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_FALSE(res->get_header_value("X-Correlation-ID").empty());
}

// ── Auth fixture: auth enabled with known token ────────────────────────────

class RoutesAuthTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  httplib::Server server_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  AuthMiddleware auth_{"secret-token-abc"};  // auth enabled
  RateLimiter rate_{1000.0};
  int port_{0};
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

void RoutesAuthTest::SetUp() {
  register_routes(server_, store_, nats_, auth_, rate_);
  port_ = server_.bind_to_any_port("127.0.0.1");
  thread_ = std::thread([this]() { server_.listen_after_bind(); });
  client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
  client_->set_connection_timeout(5);
  client_->set_read_timeout(5);
}

void RoutesAuthTest::TearDown() {
  server_.stop();
  thread_.join();
}

// Issue #40/#65: health endpoint is always accessible without auth.
TEST_F(RoutesAuthTest, HealthBypassesAuth) {
  const auto res = client_->Get("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

// Issue #40/#65: request without Authorization returns 401.
TEST_F(RoutesAuthTest, NoTokenReturns401) {
  const auto res = client_->Get("/v1/research/stats");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// Issue #40/#65: wrong token returns 401.
TEST_F(RoutesAuthTest, WrongTokenReturns401) {
  httplib::Headers headers = {{"Authorization", "Bearer wrong-token"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// Issue #40/#65: correct token passes auth.
TEST_F(RoutesAuthTest, CorrectTokenPasses) {
  httplib::Headers headers = {{"Authorization", "Bearer secret-token-abc"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

// Issue #40/#65: POST with correct token submits successfully.
TEST_F(RoutesAuthTest, PostWithCorrectTokenReturns202) {
  const std::string payload = R"({"idea":"auth test idea"})";
  httplib::Headers headers = {{"Authorization", "Bearer secret-token-abc"}};
  const auto res = client_->Post("/v1/research", headers, payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);
}

// ── Rate limiter fixture ───────────────────────────────────────────────────

class RoutesRateTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  httplib::Server server_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  AuthMiddleware auth_{""};  // auth disabled
  RateLimiter rate_{2.0};    // 2 RPS — easily exhausted
  int port_{0};
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

void RoutesRateTest::SetUp() {
  register_routes(server_, store_, nats_, auth_, rate_);
  port_ = server_.bind_to_any_port("127.0.0.1");
  thread_ = std::thread([this]() { server_.listen_after_bind(); });
  client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
  client_->set_connection_timeout(5);
  client_->set_read_timeout(5);
}

void RoutesRateTest::TearDown() {
  server_.stop();
  thread_.join();
}

// Issue #44: rapid requests should eventually get 429.
TEST_F(RoutesRateTest, RateLimitEnforcedOn429) {
  // Health is exempt, so use stats endpoint.
  bool got_429 = false;
  for (int i = 0; i < 20; ++i) {
    const auto res = client_->Get("/v1/research/stats");
    if (res && res->status == 429) {
      got_429 = true;
      break;
    }
  }
  EXPECT_TRUE(got_429) << "Expected 429 after burst, but never received it";
}

// Issue #44: health endpoint is never rate limited.
TEST_F(RoutesRateTest, HealthNotRateLimited) {
  // Even with a very low rate limit, health should always return 200.
  for (int i = 0; i < 10; ++i) {
    const auto res = client_->Get("/v1/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
  }
}

}  // namespace projectnestor::test
