// Unit tests for nestor::TlsConfig — C++20

#include "nestor/tls_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

namespace nestor::test {

// ── Environment helpers ───────────────────────────────────────────────────────

// RAII env-var setter that restores the previous value on destruction.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* prev = std::getenv(name);
    had_prev_ = (prev != nullptr);
    if (had_prev_) prev_ = prev;
    ::setenv(name, value, 1);
  }
  ~ScopedEnv() {
    if (had_prev_) {
      ::setenv(name_, prev_.c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* name_;
  std::string prev_;
  bool had_prev_{false};
};

// ── Fixture ───────────────────────────────────────────────────────────────────

class TlsConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Ensure these vars are unset at the start of each test.
    ::unsetenv("NESTOR_TLS_ENABLED");
    ::unsetenv("NESTOR_TLS_CERT");
    ::unsetenv("NESTOR_TLS_KEY");

    // Create a temporary directory for test cert/key stand-ins.
    tmp_dir_ = std::filesystem::temp_directory_path() / "nestor_tls_test";
    std::filesystem::create_directories(tmp_dir_);

    // Write minimal stand-in files (content doesn't matter for readability
    // checks — only that open() succeeds).
    cert_path_ = tmp_dir_ / "server.crt";
    key_path_ = tmp_dir_ / "server.key";
    {
      std::ofstream{cert_path_} << "CERT";
      std::ofstream{key_path_} << "KEY";
    }
  }

  void TearDown() override {
    std::filesystem::remove_all(tmp_dir_);
    ::unsetenv("NESTOR_TLS_ENABLED");
    ::unsetenv("NESTOR_TLS_CERT");
    ::unsetenv("NESTOR_TLS_KEY");
  }

  std::filesystem::path tmp_dir_;
  std::filesystem::path cert_path_;
  std::filesystem::path key_path_;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(TlsConfigTest, DefaultIsDisabled) {
  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_FALSE(cfg->enabled);
  EXPECT_TRUE(err.str().empty());
}

TEST_F(TlsConfigTest, ExplicitFalseIsDisabled) {
  ScopedEnv e("NESTOR_TLS_ENABLED", "false");
  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_FALSE(cfg->enabled);
}

TEST_F(TlsConfigTest, ZeroIsDisabled) {
  ScopedEnv e("NESTOR_TLS_ENABLED", "0");
  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_FALSE(cfg->enabled);
}

TEST_F(TlsConfigTest, EnabledWithValidFiles) {
  ScopedEnv e1("NESTOR_TLS_ENABLED", "true");
  ScopedEnv e2("NESTOR_TLS_CERT", cert_path_.c_str());
  ScopedEnv e3("NESTOR_TLS_KEY", key_path_.c_str());

  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  ASSERT_TRUE(cfg.has_value()) << "Error: " << err.str();
  EXPECT_TRUE(cfg->enabled);
  EXPECT_EQ(cfg->cert_path, cert_path_.string());
  EXPECT_EQ(cfg->key_path, key_path_.string());
  EXPECT_TRUE(err.str().empty());
}

TEST_F(TlsConfigTest, OneIsEnabled) {
  ScopedEnv e1("NESTOR_TLS_ENABLED", "1");
  ScopedEnv e2("NESTOR_TLS_CERT", cert_path_.c_str());
  ScopedEnv e3("NESTOR_TLS_KEY", key_path_.c_str());

  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->enabled);
}

TEST_F(TlsConfigTest, MalformedEnabledReturnsNullopt) {
  ScopedEnv e("NESTOR_TLS_ENABLED", "yes");
  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  EXPECT_FALSE(cfg.has_value());
  EXPECT_NE(err.str().find("not recognised"), std::string::npos);
}

TEST_F(TlsConfigTest, EnabledMissingCertReturnsNullopt) {
  ScopedEnv e1("NESTOR_TLS_ENABLED", "true");
  // NESTOR_TLS_CERT deliberately not set.
  ScopedEnv e2("NESTOR_TLS_KEY", key_path_.c_str());

  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  EXPECT_FALSE(cfg.has_value());
  EXPECT_NE(err.str().find("NESTOR_TLS_CERT"), std::string::npos);
}

TEST_F(TlsConfigTest, EnabledMissingKeyReturnsNullopt) {
  ScopedEnv e1("NESTOR_TLS_ENABLED", "true");
  ScopedEnv e2("NESTOR_TLS_CERT", cert_path_.c_str());
  // NESTOR_TLS_KEY deliberately not set.

  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  EXPECT_FALSE(cfg.has_value());
  EXPECT_NE(err.str().find("NESTOR_TLS_KEY"), std::string::npos);
}

TEST_F(TlsConfigTest, EnabledUnreadableCertReturnsNullopt) {
  ScopedEnv e1("NESTOR_TLS_ENABLED", "true");
  ScopedEnv e2("NESTOR_TLS_CERT", "/nonexistent/path/server.crt");
  ScopedEnv e3("NESTOR_TLS_KEY", key_path_.c_str());

  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  EXPECT_FALSE(cfg.has_value());
  EXPECT_NE(err.str().find("Cannot open cert"), std::string::npos);
}

TEST_F(TlsConfigTest, EnabledUnreadableKeyReturnsNullopt) {
  ScopedEnv e1("NESTOR_TLS_ENABLED", "true");
  ScopedEnv e2("NESTOR_TLS_CERT", cert_path_.c_str());
  ScopedEnv e3("NESTOR_TLS_KEY", "/nonexistent/path/server.key");

  std::ostringstream err;
  const auto cfg = TlsConfig::from_env(err);
  EXPECT_FALSE(cfg.has_value());
  EXPECT_NE(err.str().find("Cannot open key"), std::string::npos);
}

}  // namespace nestor::test
