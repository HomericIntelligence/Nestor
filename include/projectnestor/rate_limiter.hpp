#pragma once
// rate_limiter.hpp — Token-bucket rate limiter keyed per bearer token.
//
// Issue #44: No rate limiting on any endpoint; trivial DoS via request flooding.
//
// Design decision (documented for traceability):
//   Rate limiting is per bearer token (or per remote IP when auth is disabled).
//   This prevents a single authenticated client from monopolising the server.
//   The rate is configurable via NESTOR_RATE_LIMIT_RPS env-var (default: 100).
//   A simple token-bucket algorithm is used: each key gets `rps` tokens per
//   second; each request consumes one token. Bursts up to `rps` are allowed.

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "httplib.h"

namespace projectnestor {

class RateLimiter {
 public:
  // Construct from NESTOR_RATE_LIMIT_RPS env-var (default: 100 RPS).
  RateLimiter();

  // Construct with explicit RPS (primarily for tests).
  explicit RateLimiter(double rps);

  // Returns true if the request is within the rate limit for its key.
  // The key is derived from the Authorization header bearer token, or falls
  // back to the remote IP if no token is present.
  [[nodiscard]] bool allow(const httplib::Request& req);

  // Install rate-limit enforcement as a pre-routing handler (runs after auth).
  // Requests that exceed the limit receive HTTP 429.
  void install(httplib::Server& server);

 private:
  struct Bucket {
    double tokens;
    std::chrono::steady_clock::time_point last_refill;
  };

  [[nodiscard]] static std::string key_for(const httplib::Request& req);
  [[nodiscard]] bool consume_token(const std::string& key);

  double rps_;
  std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace projectnestor
