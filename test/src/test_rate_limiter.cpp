// test_rate_limiter.cpp — Unit tests for token-bucket rate limiter.
// Issue #44: Rate limiting to prevent DoS via request flooding.

#include "projectnestor/rate_limiter.hpp"

#include <gtest/gtest.h>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace projectnestor::test {

// ── Constructor Tests ───────────────────────────────────────────────────────

TEST(RateLimiterTest, DefaultConstructorWithoutEnvVar) {
  // Save current env var state and unset it.
  const char* saved = std::getenv("NESTOR_RATE_LIMIT_RPS");
  unsetenv("NESTOR_RATE_LIMIT_RPS");

  RateLimiter limiter;
  httplib::Request req;
  // Should use default (100 RPS) and allow requests.
  EXPECT_TRUE(limiter.allow(req));

  // Restore env var if it was set.
  if (saved != nullptr) {
    setenv("NESTOR_RATE_LIMIT_RPS", saved, 1);
  }
}

TEST(RateLimiterTest, DefaultConstructorWithValidEnvVar) {
  // Set env var to a valid value.
  setenv("NESTOR_RATE_LIMIT_RPS", "50.0", 1);

  RateLimiter limiter;
  httplib::Request req;
  req.remote_addr = "127.0.0.1";

  // Should use 50.0 RPS from env var.
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(limiter.allow(req));
  }
  // 51st request should fail (burst of 50 exhausted).
  EXPECT_FALSE(limiter.allow(req));

  unsetenv("NESTOR_RATE_LIMIT_RPS");
}

TEST(RateLimiterTest, DefaultConstructorWithInvalidEnvVar) {
  // Set env var to an invalid value (non-numeric).
  setenv("NESTOR_RATE_LIMIT_RPS", "invalid", 1);

  RateLimiter limiter;
  httplib::Request req;
  // Should fall back to default (100 RPS) due to invalid value.
  EXPECT_TRUE(limiter.allow(req));

  unsetenv("NESTOR_RATE_LIMIT_RPS");
}

TEST(RateLimiterTest, DefaultConstructorWithNegativeEnvVar) {
  // Set env var to a negative value.
  setenv("NESTOR_RATE_LIMIT_RPS", "-10", 1);

  RateLimiter limiter;
  httplib::Request req;
  // Should fall back to default (100 RPS) due to negative value.
  EXPECT_TRUE(limiter.allow(req));

  unsetenv("NESTOR_RATE_LIMIT_RPS");
}

TEST(RateLimiterTest, DefaultConstructorWithZeroEnvVar) {
  // Set env var to zero (invalid).
  setenv("NESTOR_RATE_LIMIT_RPS", "0", 1);

  RateLimiter limiter;
  httplib::Request req;
  // Should fall back to default (100 RPS) due to zero value.
  EXPECT_TRUE(limiter.allow(req));

  unsetenv("NESTOR_RATE_LIMIT_RPS");
}

TEST(RateLimiterTest, DefaultConstructorWithEmptyEnvVar) {
  // Set env var to empty string.
  setenv("NESTOR_RATE_LIMIT_RPS", "", 1);

  RateLimiter limiter;
  httplib::Request req;
  // Should fall back to default (100 RPS) due to empty value.
  EXPECT_TRUE(limiter.allow(req));

  unsetenv("NESTOR_RATE_LIMIT_RPS");
}

TEST(RateLimiterTest, ExplicitRpsConstructor) {
  RateLimiter limiter(10.0);
  // Should allow the first request.
  httplib::Request req;
  EXPECT_TRUE(limiter.allow(req));
}

TEST(RateLimiterTest, ConstructorWithZeroRpsUsesDefault) {
  RateLimiter limiter(0.0);
  // Zero or negative RPS should default to kDefaultRps (100).
  httplib::Request req;
  EXPECT_TRUE(limiter.allow(req));
}

TEST(RateLimiterTest, ConstructorWithNegativeRpsUsesDefault) {
  RateLimiter limiter(-50.0);
  httplib::Request req;
  EXPECT_TRUE(limiter.allow(req));
}

// ── Token Bucket Behavior ────────────────────────────────────────────────────

TEST(RateLimiterTest, FirstRequestAllowed) {
  RateLimiter limiter(10.0);
  httplib::Request req;
  EXPECT_TRUE(limiter.allow(req));
}

TEST(RateLimiterTest, BurstUpToRps) {
  // With RPS=5, we should allow 5 requests in a burst.
  RateLimiter limiter(5.0);
  httplib::Request req;
  req.remote_addr = "127.0.0.1";

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req)) << "Request " << i << " should be allowed";
  }
  // 6th request should exceed the burst.
  EXPECT_FALSE(limiter.allow(req));
}

TEST(RateLimiterTest, TokenRefillOverTime) {
  RateLimiter limiter(100.0);
  httplib::Request req;
  req.remote_addr = "127.0.0.1";

  // Exhaust the initial burst (100 tokens).
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(limiter.allow(req));
  }

  // 101st request should be denied.
  EXPECT_FALSE(limiter.allow(req));

  // Sleep briefly to allow token refill (100 RPS = 1 token per 10ms).
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Now we should have at least 2 tokens refilled; one more request allowed.
  EXPECT_TRUE(limiter.allow(req));
}

// ── Key-based Isolation ──────────────────────────────────────────────────────

TEST(RateLimiterTest, DifferentIPsHaveSeparateBuckets) {
  RateLimiter limiter(5.0);
  httplib::Request req1, req2;
  req1.remote_addr = "192.168.1.1";
  req2.remote_addr = "192.168.1.2";

  // Exhaust req1's bucket.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req1));
  }
  EXPECT_FALSE(limiter.allow(req1));

  // req2 should still have a full bucket.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req2));
  }
  EXPECT_FALSE(limiter.allow(req2));
}

TEST(RateLimiterTest, BearerTokenTakePrecedenceOverIP) {
  RateLimiter limiter(5.0);
  httplib::Request req_with_token, req_without_token;

  req_with_token.remote_addr = "192.168.1.1";
  req_with_token.set_header("Authorization", "Bearer secret-token-123");

  req_without_token.remote_addr = "192.168.1.1";

  // Exhaust the token-based bucket (5 tokens).
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req_with_token));
  }
  EXPECT_FALSE(limiter.allow(req_with_token));

  // Request without token uses IP; should start fresh.
  EXPECT_TRUE(limiter.allow(req_without_token));
}

TEST(RateLimiterTest, SameBearerTokenSharesBucket) {
  RateLimiter limiter(5.0);
  httplib::Request req1, req2;

  req1.remote_addr = "192.168.1.1";
  req1.set_header("Authorization", "Bearer same-token");

  req2.remote_addr = "192.168.1.2";
  req2.set_header("Authorization", "Bearer same-token");

  // Both requests use the same token, so they share the bucket.
  for (int i = 0; i < 5; ++i) {
    if (i % 2 == 0) {
      EXPECT_TRUE(limiter.allow(req1));
    } else {
      EXPECT_TRUE(limiter.allow(req2));
    }
  }
  // The next request from either should be denied.
  EXPECT_FALSE(limiter.allow(req1));
  EXPECT_FALSE(limiter.allow(req2));
}

// ── Edge Cases ───────────────────────────────────────────────────────────────

TEST(RateLimiterTest, MalformedAuthorizationHeaderFallsBackToIP) {
  RateLimiter limiter(5.0);
  httplib::Request req;
  req.remote_addr = "192.168.1.1";
  // Malformed: no "Bearer " prefix
  req.set_header("Authorization", "Basic xyz123");

  // Should fall back to IP-based rate limiting.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req));
  }
  EXPECT_FALSE(limiter.allow(req));
}

TEST(RateLimiterTest, EmptyAuthorizationHeaderFallsBackToIP) {
  RateLimiter limiter(5.0);
  httplib::Request req;
  req.remote_addr = "192.168.1.1";
  req.set_header("Authorization", "");

  // Should fall back to IP-based rate limiting.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req));
  }
  EXPECT_FALSE(limiter.allow(req));
}

TEST(RateLimiterTest, BearerWithJustSpaceAfterFallsBackToIP) {
  RateLimiter limiter(5.0);
  httplib::Request req1, req2;

  // "Bearer " is exactly 7 characters, same as kBearer.size()
  // The condition `auth.size() > kBearer.size()` is false (7 > 7 is false)
  // So it falls back to IP-based limiting.
  req1.remote_addr = "192.168.1.1";
  req1.set_header("Authorization", "Bearer ");

  req2.remote_addr = "192.168.1.2";
  req2.set_header("Authorization", "Bearer ");

  // Each should have separate IP-based buckets.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req1));
  }
  EXPECT_FALSE(limiter.allow(req1));

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.allow(req2));
  }
  EXPECT_FALSE(limiter.allow(req2));
}

TEST(RateLimiterTest, HighRpsAllowsMoreRequests) {
  RateLimiter limiter_high(1000.0);
  RateLimiter limiter_low(5.0);

  httplib::Request req_high, req_low;
  req_high.remote_addr = "192.168.1.1";
  req_low.remote_addr = "192.168.1.2";

  // High RPS should allow more burst requests.
  int high_count = 0;
  for (int i = 0; i < 1000; ++i) {
    if (limiter_high.allow(req_high)) {
      high_count++;
    } else {
      break;
    }
  }

  int low_count = 0;
  for (int i = 0; i < 1000; ++i) {
    if (limiter_low.allow(req_low)) {
      low_count++;
    } else {
      break;
    }
  }

  EXPECT_GT(high_count, low_count);
}

TEST(RateLimiterTest, InstallMethodIsNoOp) {
  RateLimiter limiter(10.0);
  httplib::Server server;
  // Should not crash.
  limiter.install(server);
}

}  // namespace projectnestor::test
