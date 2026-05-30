// rate_limiter.cpp — Token-bucket rate limiter implementation.
// Issue #44: prevent DoS via request flooding.

#include "projectnestor/rate_limiter.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace projectnestor {

namespace {
constexpr double kDefaultRps = 100.0;
}

RateLimiter::RateLimiter() : rps_(kDefaultRps) {
  const char* env = std::getenv("NESTOR_RATE_LIMIT_RPS");
  if (env != nullptr && env[0] != '\0') {
    try {
      const double val = std::stod(env);
      if (val > 0.0) {
        rps_ = val;
      } else {
        std::cerr << "[RateLimiter] Invalid NESTOR_RATE_LIMIT_RPS=" << env
                  << " (must be positive); using default " << kDefaultRps << "\n";
      }
    } catch (const std::exception& e) {
      std::cerr << "[RateLimiter] Invalid NESTOR_RATE_LIMIT_RPS=" << env << " (" << e.what()
                << "); using default " << kDefaultRps << "\n";
    }
  }
}

RateLimiter::RateLimiter(double rps) : rps_(rps > 0.0 ? rps : kDefaultRps) {}

std::string RateLimiter::key_for(const httplib::Request& req) {
  const std::string auth = req.get_header_value("Authorization");
  constexpr std::string_view kBearer = "Bearer ";
  if (auth.size() > kBearer.size() && auth.substr(0, kBearer.size()) == kBearer) {
    return "token:" + auth.substr(kBearer.size());
  }
  return "ip:" + req.remote_addr;
}

bool RateLimiter::consume_token(const std::string& key) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    // New key: start with a full bucket.
    buckets_.emplace(key, Bucket{rps_ - 1.0, now});
    return true;
  }

  Bucket& bucket = it->second;
  const double elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
  // Refill tokens based on elapsed time, capped at rps_ (max burst = 1 second).
  bucket.tokens = std::min(rps_, bucket.tokens + elapsed * rps_);
  bucket.last_refill = now;

  if (bucket.tokens < 1.0) {
    return false;  // Rate limit exceeded.
  }
  bucket.tokens -= 1.0;
  return true;
}

bool RateLimiter::allow(const httplib::Request& req) { return consume_token(key_for(req)); }

void RateLimiter::install(httplib::Server& server) {
  // Note: this handler runs in addition to the auth pre-routing handler.
  // httplib only supports one set_pre_routing_handler, so rate limiting is
  // integrated into routes.cpp where both auth and rate checks are applied.
  // This install() method is provided for standalone use if needed.
  (void)server;
}

}  // namespace projectnestor
