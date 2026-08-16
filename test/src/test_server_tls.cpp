// Integration test: TLS round-trip via SSLServer + SSLClient — C++20
//
// Generates a self-signed certificate in-process (no shell dependency,
// no cwd assumption) and exercises the real httplib::SSLServer path
// by starting a server on an ephemeral port and hitting /v1/health.

#include "nestor/nats_client.hpp"
#include "nestor/rate_limiter.hpp"
#include "nestor/routes.hpp"
#include "nestor/store.hpp"

#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace nestor::test {

using json = nlohmann::json;

// ── Self-signed cert generation ───────────────────────────────────────────────

namespace {

/// Write a self-signed RSA-2048 cert+key pair under @p dir.
/// Returns {cert_path, key_path}.
std::pair<std::string, std::string> generate_self_signed(const std::filesystem::path& dir) {
  // Key
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  if (pkey == nullptr) {
    throw std::runtime_error("EVP_RSA_gen failed");
  }

  // Cert
  X509* x509 = X509_new();
  if (x509 == nullptr) {
    EVP_PKEY_free(pkey);
    throw std::runtime_error("X509_new failed");
  }

  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), 365L * 24 * 3600);
  X509_set_pubkey(x509, pkey);

  X509_NAME* name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
  X509_set_issuer_name(x509, name);
  X509_sign(x509, pkey, EVP_sha256());

  const std::filesystem::path cert_path = dir / "server.crt";
  const std::filesystem::path key_path = dir / "server.key";

  // Write cert (0600: private key material must never be world-readable)
  {
    const int fd = open(cert_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
      X509_free(x509);
      EVP_PKEY_free(pkey);
      throw std::runtime_error("Cannot open cert for writing: " + cert_path.string());
    }
    FILE* f = fdopen(fd, "wb");
    if (f == nullptr) {
      close(fd);
      X509_free(x509);
      EVP_PKEY_free(pkey);
      throw std::runtime_error("Cannot open cert for writing: " + cert_path.string());
    }
    PEM_write_X509(f, x509);
    fclose(f);
  }
  // Write key (0600: private key material must never be world-readable)
  {
    const int fd = open(key_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
      X509_free(x509);
      EVP_PKEY_free(pkey);
      throw std::runtime_error("Cannot open key for writing: " + key_path.string());
    }
    FILE* f = fdopen(fd, "wb");
    if (f == nullptr) {
      close(fd);
      X509_free(x509);
      EVP_PKEY_free(pkey);
      throw std::runtime_error("Cannot open key for writing: " + key_path.string());
    }
    PEM_write_PrivateKey(f, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f);
  }

  X509_free(x509);
  EVP_PKEY_free(pkey);

  return {cert_path.string(), key_path.string()};
}

}  // namespace

// ── Fixture ───────────────────────────────────────────────────────────────────

class ServerTlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create temp dir and generate cert/key in-process.
    tmp_dir_ = std::filesystem::temp_directory_path() / "nestor_tls_integ";
    std::filesystem::create_directories(tmp_dir_);
    auto [cert, key] = generate_self_signed(tmp_dir_);
    cert_path_ = cert;
    key_path_ = key;

    // Start SSLServer on an ephemeral port.
    ssl_server_ = std::make_unique<httplib::SSLServer>(cert_path_.c_str(), key_path_.c_str());
    ASSERT_TRUE(ssl_server_->is_valid()) << "SSLServer init failed; check cert/key generation.";

    register_routes(*ssl_server_, store_, nats_, limiter_);
    port_ = ssl_server_->bind_to_any_port("127.0.0.1");
    ASSERT_GT(port_, 0);

    server_thread_ = std::thread([this]() { ssl_server_->listen_after_bind(); });
  }

  void TearDown() override {
    if (ssl_server_) {
      ssl_server_->stop();
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    std::filesystem::remove_all(tmp_dir_);
  }

  std::filesystem::path tmp_dir_;
  std::string cert_path_;
  std::string key_path_;
  Store store_;
  NatsClient nats_{"nats://localhost:1"};
  // TLS transport tests do not exercise throttling — use a permissive limiter.
  RateLimiter limiter_{[] {
    RateLimitConfig cfg;
    cfg.default_rps = 100000.0;
    cfg.default_burst = 100000.0;
    cfg.research_rps = 100000.0;
    cfg.research_burst = 100000.0;
    return cfg;
  }()};
  std::unique_ptr<httplib::SSLServer> ssl_server_;
  int port_{0};
  std::thread server_thread_;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(ServerTlsTest, HealthEndpointOverTlsReturns200) {
  // Use SSLClient with cert verification disabled (self-signed).
  httplib::SSLClient client("127.0.0.1", port_);
  client.enable_server_certificate_verification(false);
  client.set_connection_timeout(5);
  client.set_read_timeout(5);

  const auto res = client.Get("/v1/health");
  ASSERT_TRUE(res) << "Request failed (server may not have started yet)";
  EXPECT_EQ(res->status, 200);
  const auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
}

TEST_F(ServerTlsTest, PlaintextClientRejectedByTlsServer) {
  // A plain (non-TLS) client connecting to an SSLServer should fail at the
  // TLS handshake — the response will be absent or non-200.
  httplib::Client plain_client("127.0.0.1", port_);
  plain_client.set_connection_timeout(2);
  plain_client.set_read_timeout(2);

  const auto res = plain_client.Get("/v1/health");
  // Either no response (connection error) or a non-200 response.
  EXPECT_TRUE(!res || res->status != 200)
      << "Plain HTTP client should not succeed against SSLServer";
}

}  // namespace nestor::test
