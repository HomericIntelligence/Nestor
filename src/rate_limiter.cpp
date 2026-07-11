// Nestor rate limiter — token-bucket per-IP, C++20.

#include "nestor/rate_limiter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace nestor {

namespace {

/// Parse an environment variable as a strictly positive finite double.
/// Returns default_val if the variable is unset, malformed, non-positive,
/// or non-finite. Logs a WARN to stderr in the latter cases.
double parse_positive_double(const char* name, double default_val) {
  const char* env = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr) {
    return default_val;
  }
  double value = 0.0;
  try {
    std::size_t idx = 0;
    value = std::stod(std::string{env}, &idx);
    if (idx != std::string{env}.size()) {
      throw std::invalid_argument{"trailing characters"};
    }
  } catch (const std::exception& e) {
    std::cerr << "[ratelimit] WARN: " << name << "=\"" << env << "\" is not a valid number ("
              << e.what() << "); using default " << default_val << ".\n";
    return default_val;
  }
  if (value <= 0.0 || !std::isfinite(value)) {
    std::cerr << "[ratelimit] WARN: " << name << "=\"" << env
              << "\" must be positive and finite; using default " << default_val << ".\n";
    return default_val;
  }
  return value;
}

}  // namespace

// ─── RateLimitConfig ─────────────────────────────────────────────────────────

RateLimitConfig RateLimitConfig::from_env() {
  RateLimitConfig cfg;

  const char* disable_env =
      std::getenv("NESTOR_RATELIMIT_DISABLE");  // NOLINT(concurrency-mt-unsafe)
  cfg.disabled = (disable_env != nullptr && std::string{disable_env} == "1");

  cfg.default_rps = parse_positive_double("NESTOR_RATELIMIT_RPS", 10.0);
  cfg.default_burst = parse_positive_double("NESTOR_RATELIMIT_BURST", 20.0);
  cfg.research_rps = parse_positive_double("NESTOR_RATELIMIT_RESEARCH_RPS", 2.0);
  cfg.research_burst = parse_positive_double("NESTOR_RATELIMIT_RESEARCH_BURST", 5.0);

  // Enforce burst >= 1 so a freshly-constructed bucket can always admit at
  // least one request (avoids a config-induced permanent 429 on every call).
  if (cfg.default_burst < 1.0) {
    std::cerr << "[ratelimit] WARN: effective default_burst < 1; clamping to 1.\n";
    cfg.default_burst = 1.0;
  }
  if (cfg.research_burst < 1.0) {
    std::cerr << "[ratelimit] WARN: effective research_burst < 1; clamping to 1.\n";
    cfg.research_burst = 1.0;
  }

  return cfg;
}

// ─── RateLimiter ─────────────────────────────────────────────────────────────

RateLimiter::RateLimiter(RateLimitConfig cfg, NowFn now_fn)
    : cfg_{cfg}, now_fn_{std::move(now_fn)} {}

RateLimitDecision RateLimiter::check(const std::string& key, RouteClass rc) {
  // Fail-closed: route empty keys to a shared "__unknown__" bucket; never bypass.
  const std::string effective_key = key.empty() ? std::string{"__unknown__"} : key;

  // Select parameters based on route class; all cases explicit (no default:).
  double capacity = 0.0;
  double refill_rps = 0.0;
  switch (rc) {
    case RouteClass::Default:
      capacity = cfg_.default_burst;
      refill_rps = cfg_.default_rps;
      break;
    case RouteClass::Research:
      capacity = cfg_.research_burst;
      refill_rps = cfg_.research_rps;
      break;
  }

  const TimePoint now = now_fn_();

  std::lock_guard<std::mutex> lock{mutex_};

  // Insert new bucket or retrieve existing one.
  auto it = buckets_.find(effective_key);
  if (it == buckets_.end()) {
    // Evict if at capacity before inserting to bound map size.
    if (buckets_.size() >= kMaxTrackedIps) {
      evict_oldest_locked();
    }
    Bucket fresh;
    fresh.tokens = capacity;  // New bucket starts full.
    fresh.last_refill = now;
    fresh.last_seen = now;
    auto [inserted_it, _] = buckets_.emplace(effective_key, fresh);
    it = inserted_it;
  }

  Bucket& b = it->second;

  // Refill: clamp elapsed to 0 to defend against any non-monotonic edge
  // (steady_clock is contractually monotonic, but WSL2 virtualisation adds
  // a small risk; clamping is free and eliminates the edge case entirely).
  const double elapsed_sec =
      std::max(0.0, std::chrono::duration<double>(now - b.last_refill).count());
  b.tokens = std::min(capacity, b.tokens + elapsed_sec * refill_rps);
  b.last_refill = now;
  b.last_seen = now;

  RateLimitDecision d;
  if (b.tokens >= 1.0) {
    b.tokens -= 1.0;
    d.allowed = true;
    d.retry_after_sec = 0;
  } else {
    // Retry-After = ceil((1 - tokens) / rps).  Formula from §2.1 of the plan.
    const double wait = (1.0 - b.tokens) / refill_rps;
    d.allowed = false;
    d.retry_after_sec = static_cast<int>(std::ceil(wait));
    if (d.retry_after_sec < 1) {
      d.retry_after_sec = 1;  // Always at least 1 second — meaningful to clients.
    }
  }
  return d;
}

void RateLimiter::evict_oldest_locked() {
  // O(N) linear scan — see header doc comment for performance rationale.
  auto oldest = buckets_.end();
  for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
    if (oldest == buckets_.end() || it->second.last_seen < oldest->second.last_seen) {
      oldest = it;
    }
  }
  if (oldest != buckets_.end()) {
    buckets_.erase(oldest);
  }
}

}  // namespace nestor
