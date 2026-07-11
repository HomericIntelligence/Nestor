// Nestor authentication tests — C++20

#include "nestor/auth.hpp"
#include "nestor/nats_client.hpp"
#include "nestor/rate_limiter.hpp"
#include "nestor/routes.hpp"
#include "nestor/store.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace nestor::test {

using json = nlohmann::json;

class AuthTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  httplib::Server server_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  RateLimiter limiter_{[]() {
    RateLimitConfig cfg;
    cfg.default_rps = 10000.0;
    cfg.default_burst = 10000.0;
    cfg.research_rps = 10000.0;
    cfg.research_burst = 10000.0;
    cfg.disabled = false;
    return cfg;
  }()};
  int port_{0};
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

void AuthTest::SetUp() {
  // Install middleware with a known test token and Required mode.
  AuthConfig cfg{AuthMode::Required, "test-token"};
  install_auth_middleware(server_, cfg);
  register_routes(server_, store_, nats_, limiter_);

  port_ = server_.bind_to_any_port("127.0.0.1");
  thread_ = std::thread([this]() { server_.listen_after_bind(); });
  client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
  client_->set_connection_timeout(5);
  client_->set_read_timeout(5);
}

void AuthTest::TearDown() {
  server_.stop();
  thread_.join();
}

// ── Test Group 1: Missing Authorization Header ───────────────────────────────

TEST_F(AuthTest, NoAuthHeaderOnHealthReturns200) {
  const auto res = client_->Get("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
}

TEST_F(AuthTest, MissingAuthHeaderOnStatsReturns401) {
  const auto res = client_->Get("/v1/research/stats");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "unauthorized");
}

TEST_F(AuthTest, MissingAuthHeaderOnPostResearchReturns401) {
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto res = client_->Post("/v1/research", payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(AuthTest, MissingAuthHeaderOnCompleteReturns401) {
  const auto res = client_->Post("/v1/research/test-id/complete", "{}", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// ── Test Group 2: Wrong Bearer Token ──────────────────────────────────────────

TEST_F(AuthTest, WrongBearerTokenReturns401) {
  httplib::Headers headers{{"Authorization", "Bearer wrong-token"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["detail"], "unauthorized");
}

// ── Test Group 3: Malformed Authorization Header ────────────────────────────

TEST_F(AuthTest, BasicAuthReturns401) {
  httplib::Headers headers{{"Authorization", "Basic dXNlcjpwYXNz"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(AuthTest, BearerWithoutTokenReturns401) {
  httplib::Headers headers{{"Authorization", "Bearer"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(AuthTest, LowercaseBearerSchemeReturns401) {
  httplib::Headers headers{{"Authorization", "bearer test-token"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(AuthTest, BearerTokenWithEmbeddedWhitespaceReturns401) {
  httplib::Headers headers{{"Authorization", "Bearer test token"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// ── Test Group 4: Correct Bearer Token ────────────────────────────────────

TEST_F(AuthTest, HealthWithQueryStringStillReturns200) {
  // Regression: cpp-httplib strips query string from req.path
  // (httplib.h:6387-6393), so allowlist exact-match handles
  // probes like ?probe=1 or ?cache_bust=N.
  const auto res = client_->Get("/v1/health?probe=1&cache_bust=42");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(AuthTest, MissingAuthHeaderEmitsWwwAuthenticateChallenge) {
  // RFC 9110 §11.6.1: 401 responses MUST include WWW-Authenticate.
  const auto res = client_->Get("/v1/research/stats");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
  ASSERT_TRUE(res->has_header("WWW-Authenticate"));
  EXPECT_EQ(res->get_header_value("WWW-Authenticate"), R"(Bearer realm="nestor")");
}

TEST_F(AuthTest, WrongTokenAlsoEmitsWwwAuthenticateChallenge) {
  httplib::Headers headers{{"Authorization", "Bearer wrong-token"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
  ASSERT_TRUE(res->has_header("WWW-Authenticate"));
  EXPECT_EQ(res->get_header_value("WWW-Authenticate"), R"(Bearer realm="nestor")");
}

TEST_F(AuthTest, CorrectBearerTokenOnStatsReturns200) {
  httplib::Headers headers{{"Authorization", "Bearer test-token"}};
  const auto res = client_->Get("/v1/research/stats", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(AuthTest, CorrectBearerTokenOnPostResearchReturns202) {
  httplib::Headers headers{{"Authorization", "Bearer test-token"}};
  const std::string payload = R"({"idea":"test","context":"ctx"})";
  const auto res = client_->Post("/v1/research", headers, payload, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 202);
  const auto body = json::parse(res->body);
  EXPECT_FALSE(body["id"].get<std::string>().empty());
}

TEST_F(AuthTest, CorrectBearerTokenOnCompleteReturns404) {
  // Expect 404 because the ID doesn't exist, not 401.
  httplib::Headers headers{{"Authorization", "Bearer test-token"}};
  const auto res =
      client_->Post("/v1/research/nonexistent-id/complete", headers, "{}", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── Test Group 5: Config Load Tests ───────────────────────────────────────

class AuthConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Clear env vars before each test
    ::unsetenv("NESTOR_AUTH_TOKEN");
    ::unsetenv("NESTOR_AUTH_MODE");
  }
};

TEST_F(AuthConfigTest, ConfigLoad_RequiredModeWithoutTokenReturnsNullopt) {
  ::setenv("NESTOR_AUTH_MODE", "required", 1);
  // Token not set
  auto cfg = load_auth_config_from_env();
  EXPECT_FALSE(cfg.has_value());
}

TEST_F(AuthConfigTest, ConfigLoad_RequiredModeWithEmptyTokenReturnsNullopt) {
  ::setenv("NESTOR_AUTH_MODE", "required", 1);
  ::setenv("NESTOR_AUTH_TOKEN", "", 1);
  auto cfg = load_auth_config_from_env();
  EXPECT_FALSE(cfg.has_value());
}

TEST_F(AuthConfigTest, ConfigLoad_UnknownModeStringReturnsNullopt) {
  ::setenv("NESTOR_AUTH_TOKEN", "s3cret", 1);

  // Test "yes"
  ::setenv("NESTOR_AUTH_MODE", "yes", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());

  // Test "on"
  ::setenv("NESTOR_AUTH_MODE", "on", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());

  // Test "1"
  ::setenv("NESTOR_AUTH_MODE", "1", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());

  // Test "disabled"
  ::setenv("NESTOR_AUTH_MODE", "disabled", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());
}

TEST_F(AuthConfigTest, ConfigLoad_NonLowercaseModeStringReturnsNullopt) {
  ::setenv("NESTOR_AUTH_TOKEN", "s3cret", 1);

  // Test "Required"
  ::setenv("NESTOR_AUTH_MODE", "Required", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());

  // Test "REQUIRED"
  ::setenv("NESTOR_AUTH_MODE", "REQUIRED", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());

  // Test "None"
  ::setenv("NESTOR_AUTH_MODE", "None", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());

  // Test "NONE"
  ::setenv("NESTOR_AUTH_MODE", "NONE", 1);
  EXPECT_FALSE(load_auth_config_from_env().has_value());
}

TEST_F(AuthConfigTest, ConfigLoad_DefaultModeIsRequired) {
  ::setenv("NESTOR_AUTH_TOKEN", "s3cret", 1);
  // NESTOR_AUTH_MODE not set, should default to Required
  auto cfg = load_auth_config_from_env();
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->mode, AuthMode::Required);
  EXPECT_EQ(cfg->token, "s3cret");
}

TEST_F(AuthConfigTest, ConfigLoad_NoneModeAllowsMissingToken) {
  ::setenv("NESTOR_AUTH_MODE", "none", 1);
  // Token not set, but mode is "none" so it should succeed
  auto cfg = load_auth_config_from_env();
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->mode, AuthMode::None);
  EXPECT_EQ(cfg->token, "");
}

TEST_F(AuthConfigTest, ConfigLoad_RequiredModeWithTokenReturnsConfig) {
  ::setenv("NESTOR_AUTH_MODE", "required", 1);
  ::setenv("NESTOR_AUTH_TOKEN", "s3cret", 1);
  auto cfg = load_auth_config_from_env();
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->mode, AuthMode::Required);
  EXPECT_EQ(cfg->token, "s3cret");
}

// ── Test Group 6: No Token in Logs ────────────────────────────────────────

TEST_F(AuthConfigTest, NoTokenInLogsStaticCheck) {
  // Read auth.cpp and verify it doesn't log the token. The source directory is
  // injected at compile time via NESTOR_SOURCE_DIR so this works
  // regardless of the build/checkout location (e.g. CI runners).
  const std::string auth_path = std::string(NESTOR_SOURCE_DIR) + "/src/auth.cpp";
  std::ifstream auth_file(auth_path);
  ASSERT_TRUE(auth_file.is_open()) << "Cannot open " << auth_path << " for verification";

  std::string line;
  int line_num = 0;
  bool found_violation = false;
  std::ostringstream violations;

  while (std::getline(auth_file, line)) {
    ++line_num;
    // Check for log-emitting symbols with token-related identifiers in same line
    bool has_log_call =
        (line.find("std::cout") != std::string::npos ||
         line.find("std::cerr") != std::string::npos || line.find("printf") != std::string::npos ||
         line.find("fmt::print") != std::string::npos);
    bool has_token_ref = (line.find("token") != std::string::npos);

    if (has_log_call && has_token_ref) {
      found_violation = true;
      violations << "Line " << line_num << ": " << line << "\n";
    }
  }

  EXPECT_FALSE(found_violation) << "Token appears in logging statements:\n" << violations.str();
}

}  // namespace nestor::test
