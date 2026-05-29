#include "projectnestor/auth.hpp"
#include "projectnestor/nats_client.hpp"
#include "projectnestor/rate_limiter.hpp"
#include "projectnestor/routes.hpp"
#include "projectnestor/store.hpp"

#include <cstdlib>
#include <memory>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectnestor::test {

using json = nlohmann::json;

// ─── Permissive config helpers ────────────────────────────────────────────────
// High burst + high RPS so existing tests are unaffected by rate limiting.
static RateLimitConfig permissive_cfg() {
  RateLimitConfig cfg;
  cfg.default_rps = 10000.0;
  cfg.default_burst = 10000.0;
  cfg.research_rps = 10000.0;
  cfg.research_burst = 10000.0;
  cfg.disabled = false;
  return cfg;
}

// Strict config for rate-limit integration tests: research_burst=2 so the 3rd
// request triggers a 429.
static RateLimitConfig strict_research_cfg() {
  RateLimitConfig cfg;
  cfg.default_rps = 10000.0;
  cfg.default_burst = 10000.0;
  cfg.research_rps = 1.0;
  cfg.research_burst = 2.0;
  cfg.disabled = false;
  return cfg;
}

// ─── RoutesTest (permissive rate limits) ─────────────────────────────────────

class RoutesTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;
  httplib::Headers auth_headers() const {
    return httplib::Headers{{"Authorization", "Bearer test-token"}};
  }

  httplib::Server server_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  RateLimiter limiter_{permissive_cfg()};
  int port_{0};
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

void RoutesTest::SetUp() {
  ::setenv("NESTOR_AUTH_TOKEN", "test-token", 1);
  ::setenv("NESTOR_AUTH_MODE", "required", 1);

  auto auth_cfg = load_auth_config_from_env();
  ASSERT_TRUE(auth_cfg.has_value());

  install_auth_middleware(server_, *auth_cfg);
  server_.set_payload_max_length(1 * 1024 * 1024);  // 1 MiB
  register_routes(server_, store_, nats_, limiter_);
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
  const auto res = client_->Get("/v1/health", auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
}

TEST_F(RoutesTest, StatsEndpointReturnsZeros) {
  const auto res = client_->Get("/v1/research/stats", auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_FALSE(body.contains("active"));
  EXPECT_EQ(body["completed"], 0);
  EXPECT_EQ(body["pending"], 0);
}

TEST_F(RoutesTest, PostResearchReturns202) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);
  const auto body = json::parse(res->body);
  EXPECT_FALSE(body["id"].get<std::string>().empty());
  EXPECT_EQ(body["status"], "pending");
}

TEST_F(RoutesTest, PostResearchInvalidJsonReturns400) {
  const auto res = client_->Post("/v1/research", auth_headers(), "not json", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Invalid JSON");
}

TEST_F(RoutesTest, PostResearchEmptyBodyReturns400) {
  const auto res = client_->Post("/v1/research", auth_headers(), "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(RoutesTest, StatsReflectsSubmission) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  client_->Post("/v1/research", auth_headers(), payload, "application/json");

  const auto res = client_->Get("/v1/research/stats", auth_headers());
  ASSERT_TRUE(res);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["pending"], 1);
}

TEST_F(RoutesTest, CompleteResearchReturns200) {
  // Submit first to get a valid id.
  const std::string payload = R"({"idea":"test idea","context":"ctx"})";
  const auto submit_res =
      client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(submit_res);
  ASSERT_EQ(submit_res->status, 202);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  // Complete it.
  const auto complete_res =
      client_->Post("/v1/research/" + id + "/complete", auth_headers(), "", "application/json");
  ASSERT_TRUE(complete_res);
  EXPECT_EQ(complete_res->status, 200);
  const auto body = json::parse(complete_res->body);
  EXPECT_EQ(body["status"], "completed");
  EXPECT_EQ(body["id"], id);
}

TEST_F(RoutesTest, CompleteResearchUnknownIdReturns404) {
  const auto res =
      client_->Post("/v1/research/nonexistent-id/complete", auth_headers(), "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["error"], "not_found");
}

TEST_F(RoutesTest, StatsReflectsCompletion) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto submit_res =
      client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  client_->Post("/v1/research/" + id + "/complete", auth_headers(), "", "application/json");

  const auto res = client_->Get("/v1/research/stats", auth_headers());
  ASSERT_TRUE(res);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["pending"], 0);
  EXPECT_EQ(body["completed"], 1);
}

TEST_F(RoutesTest, GetResearchByIdReturnsItem) {
  const std::string payload = R"({"idea":"research idea","context":"ctx"})";
  const auto submit_res =
      client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(submit_res);
  ASSERT_EQ(submit_res->status, 202);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  const auto res = client_->Get("/v1/research/" + id, auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["id"], id);
  EXPECT_EQ(body["idea"], "research idea");
  EXPECT_EQ(body["status"], "pending");
}

TEST_F(RoutesTest, GetResearchUnknownIdReturns404) {
  const auto res = client_->Get("/v1/research/nonexistent-id", auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["error"], "not_found");
}

TEST_F(RoutesTest, GetResearchByIdNotFoundAfterCompletion) {
  // complete_research erases the item from the store; GET by id returns 404.
  const std::string payload = R"({"idea":"complete me","context":"ctx"})";
  const auto submit_res =
      client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(submit_res);
  ASSERT_EQ(submit_res->status, 202);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  const auto complete_res =
      client_->Post("/v1/research/" + id + "/complete", auth_headers(), "", "application/json");
  ASSERT_TRUE(complete_res);
  ASSERT_EQ(complete_res->status, 200);

  // Item is erased on completion; get_research returns not_found.
  const auto get_res = client_->Get("/v1/research/" + id, auth_headers());
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->status, 404);
  const auto body = json::parse(get_res->body);
  EXPECT_EQ(body["error"], "not_found");
}

TEST_F(RoutesTest, GetResearchStatsNotMatchedAsId) {
  // Guards against route-ordering regression: /stats must not be captured as :id.
  const auto res = client_->Get("/v1/research/stats", auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_FALSE(body.contains("error"));
  EXPECT_TRUE(body.contains("completed"));
  EXPECT_TRUE(body.contains("pending"));
}

TEST_F(RoutesTest, ListResearchEmptyInitially) {
  const auto res = client_->Get("/v1/research", auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["count"], 0);
  EXPECT_TRUE(body["items"].is_array());
  EXPECT_TRUE(body["items"].empty());
}

TEST_F(RoutesTest, ListResearchReturnsSubmittedItems) {
  const auto r1 =
      client_->Post("/v1/research", auth_headers(), R"({"idea":"first"})", "application/json");
  ASSERT_TRUE(r1);
  ASSERT_EQ(r1->status, 202);
  const std::string id1 = json::parse(r1->body)["id"].get<std::string>();

  const auto r2 =
      client_->Post("/v1/research", auth_headers(), R"({"idea":"second"})", "application/json");
  ASSERT_TRUE(r2);
  ASSERT_EQ(r2->status, 202);
  const std::string id2 = json::parse(r2->body)["id"].get<std::string>();

  const auto res = client_->Get("/v1/research", auth_headers());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["count"], 2);
  ASSERT_TRUE(body["items"].is_array());

  bool found1 = false;
  bool found2 = false;
  for (const auto& item : body["items"]) {
    if (item["id"] == id1) {
      found1 = true;
    }
    if (item["id"] == id2) {
      found2 = true;
    }
  }
  EXPECT_TRUE(found1);
  EXPECT_TRUE(found2);
}

TEST_F(RoutesTest, PostResearchEchoesProvidedTraceparent) {
  const std::string traceparent = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
  const std::string payload = R"({"idea":"test","context":"ctx"})";

  httplib::Headers headers = auth_headers();
  headers.insert({"traceparent", traceparent});

  const auto res = client_->Post("/v1/research", headers, payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);

  const auto x_request_id = res->get_header_value("X-Request-ID");
  EXPECT_EQ(x_request_id, "0af7651916cd43dd8448eb211c80319c");

  const auto response_traceparent = res->get_header_value("traceparent");
  EXPECT_EQ(response_traceparent, "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
}

TEST_F(RoutesTest, PostResearchGeneratesTraceIdWhenAbsent) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";

  const auto res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);

  const auto x_request_id = res->get_header_value("X-Request-ID");
  EXPECT_EQ(x_request_id.length(), 32);

  const auto response_traceparent = res->get_header_value("traceparent");
  EXPECT_TRUE(response_traceparent.find("00-") == 0);
  EXPECT_TRUE(response_traceparent.find("-01") != std::string::npos);
}

TEST_F(RoutesTest, XRequestIdFallbackIsHonored) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";

  httplib::Headers headers = auth_headers();
  headers.insert({"X-Request-ID", "0123456789abcdef0123456789abcdef"});

  const auto res = client_->Post("/v1/research", headers, payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);

  const auto x_request_id = res->get_header_value("X-Request-ID");
  EXPECT_EQ(x_request_id, "0123456789abcdef0123456789abcdef");

  const auto response_traceparent = res->get_header_value("traceparent");
  EXPECT_TRUE(response_traceparent.find("00-0123456789abcdef0123456789abcdef-") == 0);
}

TEST_F(RoutesTest, CompleteResearchEchoesIncomingTraceIdInResponse) {
  // Submit with traceparent A
  const std::string traceparent_a = "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1-bbbbbbbbbbbbbbbb-01";
  const std::string payload = R"({"idea":"test","context":"ctx"})";

  httplib::Headers submit_headers = auth_headers();
  submit_headers.insert({"traceparent", traceparent_a});

  const auto submit_res =
      client_->Post("/v1/research", submit_headers, payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  // Complete with traceparent B
  const std::string traceparent_b = "00-cccccccccccccccccccccccccccccccc-dddddddddddddddd-01";
  httplib::Headers complete_headers = auth_headers();
  complete_headers.insert({"traceparent", traceparent_b});

  const auto complete_res =
      client_->Post("/v1/research/" + id + "/complete", complete_headers, "", "application/json");
  ASSERT_TRUE(complete_res);

  // Response should echo B (incoming caller's trace)
  const auto response_traceparent = complete_res->get_header_value("traceparent");
  EXPECT_TRUE(response_traceparent.find("00-cccccccccccccccccccccccccccccccc-") == 0);
}

TEST_F(RoutesTest, HealthEndpointEchoesTraceId) {
  httplib::Headers headers = auth_headers();
  headers.insert({"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"});

  const auto res = client_->Get("/v1/health", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  const auto x_request_id = res->get_header_value("X-Request-ID");
  EXPECT_EQ(x_request_id, "0af7651916cd43dd8448eb211c80319c");
}

TEST_F(RoutesTest, StatsEndpointEchoesTraceId) {
  httplib::Headers headers = auth_headers();
  headers.insert({"X-Request-ID", "0123456789abcdef0123456789abcdef"});

  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  const auto x_request_id = res->get_header_value("X-Request-ID");
  EXPECT_EQ(x_request_id, "0123456789abcdef0123456789abcdef");
}

TEST_F(RoutesTest, PostResearchNonObjectReturns400) {
  const auto res = client_->Post("/v1/research", auth_headers(), "[1,2,3]", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("detail"));
}

TEST_F(RoutesTest, PostResearchMissingIdeaReturns400) {
  const auto res =
      client_->Post("/v1/research", auth_headers(), R"({"context":"test"})", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Missing required field: idea");
}

TEST_F(RoutesTest, PostResearchNonStringIdeaReturns400) {
  const auto res =
      client_->Post("/v1/research", auth_headers(), R"({"idea":123})", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Field 'idea' must be a string");
}

TEST_F(RoutesTest, PostResearchNonStringContextReturns400) {
  const auto res = client_->Post("/v1/research", auth_headers(), R"({"idea":"test","context":123})",
                                 "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Field 'context' must be a string");
}

TEST_F(RoutesTest, PostResearchOversizedIdeaReturns400) {
  const std::string oversized_idea(4097, 'a');
  const std::string payload = R"({"idea":")" + oversized_idea + R"("})";
  const auto res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Field 'idea' exceeds maximum length");
}

TEST_F(RoutesTest, PostResearchOversizedContextReturns400) {
  const std::string oversized_context(16385, 'a');
  const std::string payload = R"({"idea":"test","context":")" + oversized_context + R"("})";
  const auto res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "Field 'context' exceeds maximum length");
}

TEST_F(RoutesTest, PostResearchAtMaxIdeaLengthReturns202) {
  const std::string max_idea(4096, 'a');
  const std::string payload = R"({"idea":")" + max_idea + R"("})";
  const auto res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);
}

TEST_F(RoutesTest, PostResearchAtMaxContextLengthReturns202) {
  const std::string max_context(16384, 'a');
  const std::string payload = R"({"idea":"test","context":")" + max_context + R"("})";
  const auto res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);
}

TEST_F(RoutesTest, PostResearchTooManyFieldsReturns400) {
  json body;
  body["idea"] = "test";
  for (int i = 0; i < 16; ++i) {
    body["field" + std::to_string(i)] = "value";
  }
  const auto res = client_->Post("/v1/research", auth_headers(), body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(RoutesTest, PostResearchOversizedBodyReturns413) {
  const std::string oversized_body(2 * 1024 * 1024, 'a');
  const auto res =
      client_->Post("/v1/research", auth_headers(), oversized_body, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 413);
}

// ─── StrictRateLimitRoutesTest ────────────────────────────────────────────────
// Uses a strict config (research_burst=2) to verify 429 behaviour.

class StrictRateLimitRoutesTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;
  httplib::Headers auth_headers() const {
    return httplib::Headers{{"Authorization", "Bearer test-token"}};
  }

  httplib::Server server_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  RateLimiter limiter_{strict_research_cfg()};
  int port_{0};
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

void StrictRateLimitRoutesTest::SetUp() {
  ::setenv("NESTOR_AUTH_TOKEN", "test-token", 1);
  ::setenv("NESTOR_AUTH_MODE", "required", 1);

  auto auth_cfg = load_auth_config_from_env();
  ASSERT_TRUE(auth_cfg.has_value());

  install_auth_middleware(server_, *auth_cfg);
  register_routes(server_, store_, nats_, limiter_);
  port_ = server_.bind_to_any_port("127.0.0.1");
  thread_ = std::thread([this]() { server_.listen_after_bind(); });
  client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
  client_->set_connection_timeout(5);
  client_->set_read_timeout(5);
}

void StrictRateLimitRoutesTest::TearDown() {
  server_.stop();
  thread_.join();
}

// Send 3 sequential POSTs; with burst=2 the 3rd must be 429.
TEST_F(StrictRateLimitRoutesTest, PostResearchRateLimitedReturns429) {
  const std::string payload = R"({"idea":"x","context":"y"})";

  const auto r1 = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(r1);
  EXPECT_NE(r1->status, 429) << "First request should not be rate limited";

  const auto r2 = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(r2);
  EXPECT_NE(r2->status, 429) << "Second request should not be rate limited";

  const auto r3 = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(r3);
  EXPECT_EQ(r3->status, 429) << "Third request must be rate limited";

  // Retry-After header must be present and parse as a positive integer.
  const std::string retry_after = r3->get_header_value("Retry-After");
  EXPECT_FALSE(retry_after.empty()) << "Retry-After header must be present on 429";
  EXPECT_GT(std::stoi(retry_after), 0) << "Retry-After must be a positive integer";
}

// With burst=2 for research, flooding /v1/research should not exhaust the
// default bucket used for /v1/health (separate per RouteClass).
TEST_F(StrictRateLimitRoutesTest, HealthEndpointNotStarvedByResearchFlood) {
  const std::string payload = R"({"idea":"flood","context":"ctx"})";

  // Flood until we get at least one 429.
  bool got429 = false;
  for (int i = 0; i < 10; ++i) {
    const auto r = client_->Post("/v1/research", auth_headers(), payload, "application/json");
    if (r && r->status == 429) {
      got429 = true;
    }
  }
  ASSERT_TRUE(got429) << "At least one /v1/research request must be rate-limited";

  // Health endpoint should still respond 200 (uses Default bucket, not Research).
  const auto health_res = client_->Get("/v1/health", auth_headers());
  ASSERT_TRUE(health_res);
  EXPECT_EQ(health_res->status, 200) << "GET /v1/health must not be starved by /v1/research flood";
}

// Assert that the 429 body is exactly {"detail":"rate_limited"}.
TEST_F(StrictRateLimitRoutesTest, RateLimit429ResponseBodyShape) {
  const std::string payload = R"({"idea":"x","context":"y"})";

  // Drain the burst.
  for (int i = 0; i < 2; ++i) {
    client_->Post("/v1/research", auth_headers(), payload, "application/json");
  }

  const auto r = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 429);

  const auto body = json::parse(r->body, nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(body.is_discarded()) << "429 body must be valid JSON";
  ASSERT_TRUE(body.contains("detail")) << "429 body must contain 'detail' key";
  EXPECT_EQ(body["detail"].get<std::string>(), "rate_limited");
}

}  // namespace projectnestor::test
