#include "projectnestor/nats_client.hpp"

#include <memory>

#include <gtest/gtest.h>

namespace projectnestor::test {

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

}  // namespace projectnestor::test
