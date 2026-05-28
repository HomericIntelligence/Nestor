#include "projectnestor/auth.hpp"
#include "projectnestor/nats_client.hpp"
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
  register_routes(server_, store_, nats_);
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

  const auto submit_res = client_->Post("/v1/research", submit_headers, payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  // Complete with traceparent B
  const std::string traceparent_b = "00-cccccccccccccccccccccccccccccccc-dddddddddddddddd-01";
  httplib::Headers complete_headers = auth_headers();
  complete_headers.insert({"traceparent", traceparent_b});

  const auto complete_res = client_->Post("/v1/research/" + id + "/complete", complete_headers, "", "application/json");
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

}  // namespace projectnestor::test
