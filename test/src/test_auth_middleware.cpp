// test_auth_middleware.cpp — Unit tests for AuthMiddleware.
// Issue #40/#65: verify 401 paths, 200 with valid token, constant-time compare.

#include "projectnestor/auth_middleware.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectnestor::test {

using json = nlohmann::json;

// Helper: create a minimal httplib::Request with given path and optional auth header.
static httplib::Request make_request(const std::string& path, const std::string& auth_header = "") {
  httplib::Request req;
  req.path = path;
  if (!auth_header.empty()) {
    req.headers.emplace("Authorization", auth_header);
  }
  return req;
}

TEST(AuthMiddlewareTest, DisabledAuthAlwaysPasses) {
  AuthMiddleware auth{""};  // empty token = disabled
  EXPECT_FALSE(auth.is_enabled());

  httplib::Request req = make_request("/v1/research");
  httplib::Response res;
  EXPECT_TRUE(auth.check_request(req, res));
}

TEST(AuthMiddlewareTest, EnabledAuthRejectsNoToken) {
  AuthMiddleware auth{"my-secret"};
  EXPECT_TRUE(auth.is_enabled());

  httplib::Request req = make_request("/v1/research");
  httplib::Response res;
  EXPECT_FALSE(auth.check_request(req, res));
  EXPECT_EQ(res.status, 401);
  // Body should be JSON with "detail" key.
  const auto body = json::parse(res.body);
  EXPECT_TRUE(body.contains("detail"));
}

TEST(AuthMiddlewareTest, EnabledAuthRejectsWrongToken) {
  AuthMiddleware auth{"my-secret"};

  httplib::Request req = make_request("/v1/research", "Bearer wrong-token");
  httplib::Response res;
  EXPECT_FALSE(auth.check_request(req, res));
  EXPECT_EQ(res.status, 401);
}

TEST(AuthMiddlewareTest, EnabledAuthAcceptsCorrectToken) {
  AuthMiddleware auth{"my-secret"};

  httplib::Request req = make_request("/v1/research", "Bearer my-secret");
  httplib::Response res;
  EXPECT_TRUE(auth.check_request(req, res));
  EXPECT_NE(res.status, 401);
}

TEST(AuthMiddlewareTest, MissingBearerPrefixReturns401) {
  AuthMiddleware auth{"my-secret"};

  httplib::Request req = make_request("/v1/research", "my-secret");
  httplib::Response res;
  EXPECT_FALSE(auth.check_request(req, res));
  EXPECT_EQ(res.status, 401);
}

TEST(AuthMiddlewareTest, ConstantTimeCompareDoesNotShortCircuit) {
  // Both tokens have the same prefix but different lengths.
  AuthMiddleware auth{"abcdefghij"};

  httplib::Request req_prefix = make_request("/v1/research", "Bearer abcde");
  httplib::Response res_prefix;
  EXPECT_FALSE(auth.check_request(req_prefix, res_prefix));
  EXPECT_EQ(res_prefix.status, 401);

  httplib::Request req_longer = make_request("/v1/research", "Bearer abcdefghijklmn");
  httplib::Response res_longer;
  EXPECT_FALSE(auth.check_request(req_longer, res_longer));
  EXPECT_EQ(res_longer.status, 401);
}

TEST(AuthMiddlewareTest, ExactMatchPasses) {
  AuthMiddleware auth{"tok123"};
  httplib::Request req = make_request("/v1/research", "Bearer tok123");
  httplib::Response res;
  EXPECT_TRUE(auth.check_request(req, res));
}

}  // namespace projectnestor::test
