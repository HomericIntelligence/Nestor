#include "nestor/reconnect_policy.hpp"

#include <gtest/gtest.h>

namespace nestor::test {

// Use a seeded RNG for deterministic results.
static std::mt19937 seeded_rng(42);

TEST(ReconnectPolicyTest, AttemptZeroIsBase) {
  ReconnectPolicy policy{std::chrono::milliseconds{100}, std::chrono::milliseconds{2000}, 1.0, 1.0};
  std::mt19937 rng(0);
  const auto d = next_delay(policy, 0, rng);
  // With jitter_lo == jitter_hi == 1.0 the result is exactly the base.
  EXPECT_EQ(d.count(), 100);
}

TEST(ReconnectPolicyTest, ExponentialGrowth) {
  // No jitter (lo == hi == 1.0) so we can verify the exact doubling.
  ReconnectPolicy policy{std::chrono::milliseconds{100}, std::chrono::milliseconds{99999}, 1.0,
                         1.0};
  std::mt19937 rng(0);
  const auto d0 = next_delay(policy, 0, rng);  // 100 * 2^0 = 100
  const auto d1 = next_delay(policy, 1, rng);  // 100 * 2^1 = 200
  const auto d2 = next_delay(policy, 2, rng);  // 100 * 2^2 = 400
  const auto d3 = next_delay(policy, 3, rng);  // 100 * 2^3 = 800
  EXPECT_EQ(d0.count(), 100);
  EXPECT_EQ(d1.count(), 200);
  EXPECT_EQ(d2.count(), 400);
  EXPECT_EQ(d3.count(), 800);
}

TEST(ReconnectPolicyTest, CapEnforced) {
  ReconnectPolicy policy{std::chrono::milliseconds{100}, std::chrono::milliseconds{500}, 1.0, 1.0};
  std::mt19937 rng(0);
  // Attempt 10 is well above the cap.
  const auto d = next_delay(policy, 10, rng);
  EXPECT_LE(d.count(), 500);
  EXPECT_GE(d.count(), 100);
}

TEST(ReconnectPolicyTest, JitterBounds) {
  ReconnectPolicy policy{std::chrono::milliseconds{100}, std::chrono::milliseconds{2000}, 0.5, 1.5};
  std::mt19937 rng(12345);
  // Sample many times and verify all results stay within [base, cap].
  for (int i = 0; i < 1000; ++i) {
    const auto d = next_delay(policy, i % 5, rng);
    EXPECT_GE(d.count(), policy.base.count()) << "attempt=" << i;
    EXPECT_LE(d.count(), policy.cap.count()) << "attempt=" << i;
  }
}

TEST(ReconnectPolicyTest, DefaultPolicyValuesAreReasonable) {
  ReconnectPolicy policy{};
  EXPECT_EQ(policy.base.count(), 100);
  EXPECT_EQ(policy.cap.count(), 2000);
  EXPECT_DOUBLE_EQ(policy.jitter_lo, 0.5);
  EXPECT_DOUBLE_EQ(policy.jitter_hi, 1.5);
}

}  // namespace nestor::test
