// Unit tests for nestor::log_security_posture — C++20

#include "nestor/security_warnings.hpp"

#include <sstream>

#include <gtest/gtest.h>

namespace nestor::test {

TEST(SecurityWarningsTest, LoopbackAndTlsNoOutput) {
  std::ostringstream out;
  log_security_posture(out, "127.0.0.1", /*tls_enabled=*/true);
  EXPECT_TRUE(out.str().empty());
}

TEST(SecurityWarningsTest, WideBIndAndTlsDisabledCompoundWarning) {
  std::ostringstream out;
  log_security_posture(out, "0.0.0.0", /*tls_enabled=*/false);
  const std::string msg = out.str();
  EXPECT_NE(msg.find("[SECURITY]"), std::string::npos);
  EXPECT_NE(msg.find("0.0.0.0"), std::string::npos);
  EXPECT_NE(msg.find("cleartext"), std::string::npos);
}

TEST(SecurityWarningsTest, WideBIndAndTlsEnabledBindOnlyWarning) {
  std::ostringstream out;
  log_security_posture(out, "0.0.0.0", /*tls_enabled=*/true);
  const std::string msg = out.str();
  EXPECT_NE(msg.find("[SECURITY]"), std::string::npos);
  EXPECT_NE(msg.find("0.0.0.0"), std::string::npos);
  // No mention of cleartext when TLS is on.
  EXPECT_EQ(msg.find("cleartext"), std::string::npos);
}

TEST(SecurityWarningsTest, LoopbackAndTlsDisabledTlsOnlyWarning) {
  std::ostringstream out;
  log_security_posture(out, "127.0.0.1", /*tls_enabled=*/false);
  const std::string msg = out.str();
  EXPECT_NE(msg.find("[SECURITY]"), std::string::npos);
  EXPECT_NE(msg.find("cleartext"), std::string::npos);
  // No mention of address binding when bind is loopback.
  EXPECT_EQ(msg.find("all interfaces"), std::string::npos);
}

}  // namespace nestor::test
