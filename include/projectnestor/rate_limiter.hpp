#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nestor {

/// Route classification for per-route rate-limit overrides.
/// No default case in switch statements — all variants are handled explicitly
/// (per the debugging-auth-mode-case-mismatch-fail-open team principle).
enum class RouteClass {
  Default,   ///< General endpoints (health, stats).
  Research,  ///< POST /v1/research — expensive write path (DoS vector per issue #44).
};

/// Configuration for the rate limiter. Always constructed via from_env().
/// Invariants (enforced by from_env()): rps > 0, burst >= 1 for both tiers.
struct RateLimitConfig {
  double default_rps = 10.0;
  double default_burst = 20.0;
  double research_rps = 2.0;
  double research_burst = 5.0;
  bool disabled = false;

  /// Read config from environment variables.
  /// Falls back to defaults on missing/malformed/non-positive values and logs a WARN.
  static RateLimitConfig from_env();
};

/// Per-client decision returned by RateLimiter::check().
struct RateLimitDecision {
  bool allowed = true;
  int retry_after_sec = 0;  ///< Populated (>= 1) only when allowed == false.
};

/// Thread-safe per-IP token-bucket rate limiter.
///
/// ## Lifetime invariant
/// Construct on the main() stack before register_routes() and before
/// server.listen(). Lambdas in routes.cpp capture `RateLimiter* lp = &limiter`
/// by value — same pattern as Store*/NatsClient* captures — ensuring the
/// pointer remains valid for the entire server lifetime (listen() blocks until
/// shutdown). See cpp-httplib-lambda-capture-ub team skill.
///
/// ## Eviction policy
/// The bucket map is capped at kMaxTrackedIps entries. When the cap is reached
/// and a new key would be inserted, a linear O(N) scan evicts the entry with
/// the oldest last_seen timestamp. This is approximate LRU. O(N) per eviction
/// is acceptable because evictions are rare relative to normal traffic; if
/// contention or latency becomes measurable under sustained >10k distinct-IP
/// floods, upgrade to a sharded map (documented as a comment, not built here).
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using NowFn = std::function<TimePoint()>;

  static constexpr std::size_t kMaxTrackedIps = 10'000;

  /// @param cfg      Rate limit configuration (must satisfy invariants).
  /// @param now_fn   Injectable clock; defaults to steady_clock::now().
  ///                 Override in unit tests for deterministic timing.
  // clang-format off
  explicit RateLimiter(RateLimitConfig cfg, NowFn now_fn = []() { return Clock::now(); });
  // clang-format on

  /// Check and consume a token for the given client key.
  ///
  /// @param key  Typically req.remote_addr. Empty string is routed to the
  ///             "__unknown__" shared bucket (fail-closed — never bypassed).
  /// @param rc   Route class; selects the appropriate token-bucket parameters.
  [[nodiscard]] RateLimitDecision check(const std::string& key, RouteClass rc);

 private:
  struct Bucket {
    double tokens = 0.0;
    TimePoint last_refill;
    TimePoint last_seen;
  };

  void evict_oldest_locked();  ///< Called holding mutex_; O(N) linear scan.

  RateLimitConfig cfg_;
  NowFn now_fn_;
  std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace nestor
