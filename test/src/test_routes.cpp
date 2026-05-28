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
  const auto submit_res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
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
  const auto res = client_->Post("/v1/research/nonexistent-id/complete", auth_headers(), "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["error"], "not_found");
}

TEST_F(RoutesTest, StatsReflectsCompletion) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto submit_res = client_->Post("/v1/research", auth_headers(), payload, "application/json");
  ASSERT_TRUE(submit_res);
  const std::string id = json::parse(submit_res->body)["id"].get<std::string>();

  client_->Post("/v1/research/" + id + "/complete", auth_headers(), "", "application/json");

  const auto res = client_->Get("/v1/research/stats", auth_headers());
  ASSERT_TRUE(res);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["pending"], 0);
  EXPECT_EQ(body["completed"], 1);
}

}  // namespace projectnestor::test
