#include "projectnestor/nats_client.hpp"

#include <chrono>
#include <memory>

#include <gtest/gtest.h>

namespace projectnestor::test {

// ─── Existing tests (all preserved) ──────────────────────────────────────────

TEST(NatsClientTest, ConstructorSetsDisconnected) {
  NatsClient client("nats://localhost:4222");
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, CloseWhenNotConnected) {
  NatsClient client("nats://localhost:4222");
  client.close();
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, PublishWhenNotConnectedReturnsFalse) {
  NatsClient client("nats://localhost:4222");
  EXPECT_FALSE(client.publish("hi.research.test", R"({"test":true})"));
}

TEST(NatsClientTest, EnsureStreamsWhenNotConnectedReturnsEarly) {
  NatsClient client("nats://localhost:4222");
  client.ensure_streams();
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, DestructorWhenNotConnected) {
  auto ptr = std::make_unique<NatsClient>("nats://localhost:4222");
  EXPECT_FALSE(ptr->is_connected());
  ptr.reset();
  SUCCEED();
}

// ConnectToInvalidUrlFails — uses 127.0.0.1:1 (closed port).
// With SetTimeout(500ms), the TCP RST returns near-instantly so the test
// remains sub-second. The SetTimeout choice is documented in issue #47 §R1-#3.
TEST(NatsClientTest, ConnectToInvalidUrlFails) {
  NatsClient client("nats://127.0.0.1:1");
  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, DoubleCloseIsSafe) {
  NatsClient client("nats://localhost:4222");
  client.close();
  client.close();
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, PublishLogWhenNotConnectedIsNoOp) {
  // publish_log must not throw or crash when NATS is unavailable.
  NatsClient client("nats://127.0.0.1:1");
  EXPECT_NO_THROW(client.publish_log("hi.logs.nestor.research_submitted", "info",
                                     "Research submitted: topic=test",
                                     nlohmann::json{{"research_id", "abc"}, {"topic", "test"}}));
}

TEST(NatsClientTest, PublishLogWithTraceIdIsNoOpWhenDisconnected) {
  // publish_log with trace_id must not throw or crash when NATS is unavailable.
  NatsClient client("nats://127.0.0.1:1");
  EXPECT_NO_THROW(client.publish_log("hi.logs.nestor.research_submitted", "info",
                                     "Research submitted: topic=test",
                                     nlohmann::json{{"research_id", "abc"}, {"topic", "test"}},
                                     "0123456789abcdef0123456789abcdef"));
}

// ─── New lifecycle tests (issue #47) ─────────────────────────────────────────

// After connect() to an unreachable URL, is_connected() stays false and the
// background reconnect thread is started but not joinable beyond destructor.
TEST(NatsClientTest, ConnectToInvalidUrlStartsBackgroundReconnect) {
  NatsClient client("nats://127.0.0.1:1");
  const bool result = client.connect();
  EXPECT_FALSE(result);
  EXPECT_FALSE(client.is_connected());
  // Destructor must return in < 1 s (verified by test runner timeout).
}

// close() interrupts the reconnect loop quickly — under 500 ms.
TEST(NatsClientTest, CloseInterruptsReconnectLoop) {
  NatsClient client("nats://127.0.0.1:1");
  client.connect();
  EXPECT_FALSE(client.is_connected());

  const auto t0 = std::chrono::steady_clock::now();
  client.close();
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500)
      << "close() took too long; reconnect loop not interrupted promptly";
  EXPECT_FALSE(client.is_connected());
}

// Destructor on a client that is actively retrying must not hang.
TEST(NatsClientTest, DestructorWhileReconnectingIsSafe) {
  auto client = std::make_unique<NatsClient>("nats://127.0.0.1:1");
  client->connect();
  EXPECT_FALSE(client->is_connected());
  // Reset (destructor) must complete without hanging.
  client.reset();
  SUCCEED();
}

// Multiple connect→close cycles against an unreachable URL.
TEST(NatsClientTest, RepeatedConnectCloseCycles) {
  for (int i = 0; i < 5; ++i) {
    NatsClient client("nats://127.0.0.1:1");
    client.connect();
    EXPECT_FALSE(client.is_connected()) << "cycle " << i;
    client.close();
    EXPECT_FALSE(client.is_connected()) << "cycle " << i;
  }
}

}  // namespace projectnestor::test
