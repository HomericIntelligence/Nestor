#pragma once

#include <algorithm>
#include <chrono>
#include <random>

namespace nestor {

/// Exponential-backoff-with-jitter parameters for NATS reconnect and
/// JetStream provisioner retry loops.
///
/// Formula: delay = clamp(base * 2^attempt, base, cap) * U(jitter_lo, jitter_hi)
struct ReconnectPolicy {
  std::chrono::milliseconds base{100};
  std::chrono::milliseconds cap{2000};
  double jitter_lo{0.5};
  double jitter_hi{1.5};
};

/// Compute the next backoff delay for a given attempt index.
/// @param policy  Backoff parameters.
/// @param attempt Zero-based attempt counter (0 = first retry).
/// @param rng     Caller-owned Mersenne-Twister; must be seeded.
/// @return        Clamped, jittered delay.
inline std::chrono::milliseconds next_delay(const ReconnectPolicy& policy, unsigned attempt,
                                            std::mt19937& rng) {
  // Compute exponential component: base * 2^attempt, capped.
  // Use double arithmetic to avoid overflow on large attempt counts.
  const double base_ms = static_cast<double>(policy.base.count());
  const double cap_ms = static_cast<double>(policy.cap.count());
  // 2^attempt growth, capped before multiplication to stay in double range.
  const double exp_factor = std::min(std::pow(2.0, static_cast<double>(attempt)), cap_ms / base_ms);
  const double uncapped = base_ms * exp_factor;
  const double capped = std::min(uncapped, cap_ms);

  // Apply jitter: multiply by a uniform sample in [jitter_lo, jitter_hi].
  std::uniform_real_distribution<double> dist(policy.jitter_lo, policy.jitter_hi);
  const double jittered = capped * dist(rng);

  // Clamp result to [base, cap] after jitter (jitter_lo < 1 could go below base).
  const double final_ms = std::clamp(jittered, base_ms, cap_ms);
  return std::chrono::milliseconds(static_cast<long long>(final_ms));
}

}  // namespace nestor
