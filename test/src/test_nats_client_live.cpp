// Live-broker integration tests for NatsClient (issue #119).
//
// These tests exercise the code paths in src/nats_client.cpp that can only
// execute against a real NATS+JetStream broker: the try_connect_once()
// success block, the external reconnect loop, disconnect/reconnect callbacks,
// JetStream provisioning (ensure_streams), publish/publish_log happy paths,
// and research-status delivery via shim_research_message.
//
// Gating: every test skips when NESTOR_LIVE_NATS_URL is unset, so the default
// ctest presets stay green for developers without docker. When the variable
// IS set (as the nats-integration-tests CI job does), an unreachable broker
// is a hard failure — no silent pass.
//
// Broker-bounce tests additionally require NESTOR_LIVE_NATS_COMPOSE (path to
// test/docker/docker-compose.nats.yml) and shell out to docker compose to
// stop/start the broker mid-test. They run last and restore the broker even
// on failure so earlier re-runs see a live broker.

#include "nestor/nats_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace nestor::test {
namespace {

using namespace std::chrono_literals;

// Poll pred every 100 ms until true or timeout; returns final pred value.
bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(100ms);
  }
  return pred();
}

const char* live_url() {
  return std::getenv("NESTOR_LIVE_NATS_URL");
}  // NOLINT(concurrency-mt-unsafe)
const char* compose_file() {
  return std::getenv("NESTOR_LIVE_NATS_COMPOSE");
}  // NOLINT(concurrency-mt-unsafe)

// Run a docker-compose lifecycle command against the test broker.
// std::system is acceptable here: test-only helper, arguments are
// build-controlled env values, never user input.
bool compose_ctl(const std::string& verb) {
  const std::string cmd =
      "docker compose -f \"" + std::string(compose_file()) + "\" " + verb + " nats";
  return std::system(cmd.c_str()) == 0;  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
}

// Unique per-call suffix so subjects never collide across tests or runs
// (the broker's JetStream state persists between test invocations).
std::string unique_suffix() {
  static std::mutex mu;
  std::scoped_lock lk(mu);
  static std::mt19937_64 rng{std::random_device{}()};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::to_string(now) + "-" + std::to_string(rng() % 1000000);
}

// Thread-safe recorder for messages delivered on the nats.c callback thread.
// The hi.research.> subscription also receives the readiness-probe publishes,
// so assertions must filter by exact subject rather than take the first
// received message.
class Recorder {
 public:
  void record(const std::string& subject, const std::string& payload) {
    std::scoped_lock lk(mu_);
    msgs_.emplace_back(subject, payload);
  }

  bool has(const std::string& subject) {
    std::scoped_lock lk(mu_);
    for (const auto& [s, p] : msgs_) {
      if (s == subject) {
        return true;
      }
    }
    return false;
  }

  std::string payload_of(const std::string& subject) {
    std::scoped_lock lk(mu_);
    for (const auto& [s, p] : msgs_) {
      if (s == subject) {
        return p;
      }
    }
    return {};
  }

 private:
  std::mutex mu_;
  std::vector<std::pair<std::string, std::string>> msgs_;
};

// Restores the broker (docker compose start) on scope exit, even when a
// bounce test fails mid-way, so later tests and re-runs see a live broker.
class BrokerRestoreGuard {
 public:
  BrokerRestoreGuard() = default;
  BrokerRestoreGuard(const BrokerRestoreGuard&) = delete;
  BrokerRestoreGuard& operator=(const BrokerRestoreGuard&) = delete;
  ~BrokerRestoreGuard() {
    if (!compose_ctl("start")) {
      // Destructor must not throw; surface the problem in the test log.
      ADD_FAILURE() << "failed to restore NATS broker via docker compose start";
    }
  }
};

class NatsClientLiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* url = live_url();
    if (url == nullptr) {
      GTEST_SKIP() << "NESTOR_LIVE_NATS_URL not set — live NATS suite skipped";
    }
    url_ = url;
  }

  // Broker-bounce tests call this first; they need docker compose control.
  void require_compose_ctl() {
    if (compose_file() == nullptr) {
      GTEST_SKIP() << "NESTOR_LIVE_NATS_COMPOSE not set — broker-bounce test skipped";
    }
  }

  // Wait until the client's provisioner has created the JetStream context and
  // streams: the first successful publish proves jsCtx + ensure_streams() +
  // the js_Publish happy path all completed. Because provisioning and
  // publish() serialize on the same mutex, a successful publish also
  // guarantees the hi.research.> research subscription exists.
  static bool wait_provisioned(NatsClient& client, const std::string& probe_subject) {
    return wait_for([&] { return client.publish(probe_subject, R"({"probe":true})"); }, 30s);
  }

  std::string url_;
};

// Covers the try_connect_once() success block and connect()'s ok-return.
TEST_F(NatsClientLiveTest, ConnectSucceedsAgainstLiveBroker) {
  NatsClient client(url_);
  EXPECT_TRUE(client.connect());
  EXPECT_TRUE(client.is_connected());
  client.close();
  EXPECT_FALSE(client.is_connected());
}

// Covers provisioner-thread JetStream context creation, ensure_streams()
// stream creation, and the js_Publish happy path in publish().
TEST_F(NatsClientLiveTest, PublishSucceedsAfterProvisioning) {
  NatsClient client(url_);
  ASSERT_TRUE(client.connect());
  const std::string subject = "hi.research.live." + unique_suffix();
  EXPECT_TRUE(wait_provisioned(client, subject))
      << "publish never succeeded — JetStream provisioning did not complete";
  client.close();
}

// A second client's ensure_streams() runs against already-existing streams
// (the "stream exists" non-fatal branch) and must still reach publish-success.
TEST_F(NatsClientLiveTest, SecondClientHitsStreamAlreadyExists) {
  NatsClient first(url_);
  ASSERT_TRUE(first.connect());
  ASSERT_TRUE(wait_provisioned(first, "hi.research.live.first-" + unique_suffix()));

  NatsClient second(url_);
  ASSERT_TRUE(second.connect());
  EXPECT_TRUE(wait_provisioned(second, "hi.research.live.second-" + unique_suffix()))
      << "second client failed to provision against pre-existing streams";

  second.close();
  first.close();
}

// Covers shim_research_message delivery and the resubscribe_research_locked
// happy path: a message published on hi.research.> reaches the handler with
// exact subject and payload.
TEST_F(NatsClientLiveTest, ResearchStatusDelivered) {
  NatsClient client(url_);
  Recorder rec;
  client.set_research_status_handler(
      [&rec](const std::string& subject, const std::string& payload) {
        rec.record(subject, payload);
      });
  ASSERT_TRUE(client.connect());
  ASSERT_TRUE(wait_provisioned(client, "hi.research.live.probe-" + unique_suffix()));

  const std::string uid = unique_suffix();
  const std::string subject = "hi.research.status." + uid;
  const std::string payload = R"({"status":"completed","research_id":")" + uid + R"("})";
  ASSERT_TRUE(client.publish(subject, payload));

  ASSERT_TRUE(wait_for([&] { return rec.has(subject); }, 10s))
      << "research-status message was not delivered to the handler";
  EXPECT_EQ(rec.payload_of(subject), payload);
  client.close();
}

// Covers the catch block in shim_research_message: a throwing handler must
// not kill the delivery thread — subsequent messages are still delivered.
TEST_F(NatsClientLiveTest, ResearchHandlerExceptionIsCaught) {
  NatsClient client(url_);
  Recorder rec;
  std::atomic<int> throw_count{0};
  const std::string uid = unique_suffix();
  const std::string throw_subject = "hi.research.status.throw-" + uid;
  const std::string ok_subject = "hi.research.status.ok-" + uid;
  client.set_research_status_handler([&](const std::string& subject, const std::string& payload) {
    if (subject == throw_subject) {
      throw_count.fetch_add(1);
      throw std::runtime_error("live-test handler failure");
    }
    rec.record(subject, payload);
  });
  ASSERT_TRUE(client.connect());
  ASSERT_TRUE(wait_provisioned(client, "hi.research.live.probe-" + unique_suffix()));

  ASSERT_TRUE(client.publish(throw_subject, R"({"boom":true})"));
  ASSERT_TRUE(client.publish(ok_subject, R"({"ok":true})"));

  ASSERT_TRUE(wait_for([&] { return rec.has(ok_subject); }, 10s))
      << "message after a throwing handler was not delivered";
  EXPECT_GE(throw_count.load(), 1) << "throwing message never reached the handler";
  EXPECT_FALSE(rec.has(throw_subject)) << "throwing handler unexpectedly recorded its message";
  client.close();
}

// Covers the publish_log() happy path: the structured JSON envelope reaches
// a subscriber with all documented fields populated.
TEST_F(NatsClientLiveTest, PublishLogDeliversStructuredJson) {
  NatsClient client(url_);
  Recorder rec;
  client.set_research_status_handler(
      [&rec](const std::string& subject, const std::string& payload) {
        rec.record(subject, payload);
      });
  ASSERT_TRUE(client.connect());
  ASSERT_TRUE(wait_provisioned(client, "hi.research.live.probe-" + unique_suffix()));

  // publish_log uses core (non-JetStream) publish; route it through the
  // hi.research.> subscription so the recorder observes it.
  const std::string subject = "hi.research.status.log-" + unique_suffix();
  const std::string trace_id = "0123456789abcdef0123456789abcdef";
  client.publish_log(subject, "info", "live-test", nlohmann::json{{"k", "v"}}, trace_id);

  ASSERT_TRUE(wait_for([&] { return rec.has(subject); }, 10s))
      << "publish_log message was not delivered";
  const nlohmann::json parsed = nlohmann::json::parse(rec.payload_of(subject));
  EXPECT_EQ(parsed.at("service"), "nestor");
  EXPECT_EQ(parsed.at("level"), "info");
  EXPECT_EQ(parsed.at("message"), "live-test");
  EXPECT_EQ(parsed.at("trace_id"), trace_id);
  EXPECT_EQ(parsed.at("metadata").at("k"), "v");
  EXPECT_TRUE(parsed.at("timestamp").is_number());
  EXPECT_GT(parsed.at("timestamp").get<double>(), 0.0);
  client.close();
}

// ─── Provisioning-failure classification (issue #121) ────────────────────────

// RAII owner of a raw broker-admin connection + jsCtx for manipulating
// JetStream state out-of-band, plus guaranteed conflict-stream cleanup.
class BrokerAdmin {
 public:
  explicit BrokerAdmin(const std::string& url) {
    if (natsConnection_ConnectTo(&conn_, url.c_str()) == NATS_OK) {
      jsOptions opts;
      jsOptions_Init(&opts);
      natsConnection_JetStream(&js_, conn_, &opts);
    }
  }
  ~BrokerAdmin() {
    if (js_ != nullptr) {
      delete_stream(kConflictName);  // never leave the conflict behind
      jsCtx_Destroy(js_);
    }
    if (conn_ != nullptr) {
      natsConnection_Destroy(conn_);
    }
  }
  BrokerAdmin(const BrokerAdmin&) = delete;
  BrokerAdmin& operator=(const BrokerAdmin&) = delete;

  bool ok() const { return js_ != nullptr; }

  bool delete_stream(const char* name) {
    jsErrCode jerr = static_cast<jsErrCode>(0);
    const natsStatus s = js_DeleteStream(js_, name, nullptr, &jerr);
    return s == NATS_OK || s == NATS_NOT_FOUND;
  }

  bool create_conflict_stream() {
    jsStreamConfig cfg;
    jsStreamConfig_Init(&cfg);
    cfg.Name = kConflictName;
    const char* subjects[] = {"hi.research.>"};
    cfg.Subjects = subjects;
    cfg.SubjectsLen = 1;
    cfg.Storage = js_MemoryStorage;
    jsStreamInfo* si = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    const natsStatus s = js_AddStream(&si, js_, &cfg, nullptr, &jerr);
    jsStreamInfo_Destroy(si);
    return s == NATS_OK;
  }

  bool stream_exists(const char* name) {
    jsStreamInfo* si = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    const natsStatus s = js_GetStreamInfo(&si, js_, name, nullptr, &jerr);
    jsStreamInfo_Destroy(si);
    return s == NATS_OK;
  }

  static constexpr const char* kConflictName = "live-conflict-121";

 private:
  natsConnection* conn_ = nullptr;
  jsCtx* js_ = nullptr;
};

// A subject-overlap conflict (jsErrCode 10065) must be classified as a real
// provisioning failure — NOT "stream exists" — and the provisioner must keep
// retrying until the conflict clears (issue #121). On the pre-fix code the
// generation is marked provisioned on the first pass, homeric-research is
// never created, and the final wait times out.
TEST_F(NatsClientLiveTest, SubjectOverlapFailsProvisioningUntilConflictRemoved) {
  BrokerAdmin admin(url_);
  ASSERT_TRUE(admin.ok()) << "broker admin connection failed";
  // Clear homeric-research so the conflict stream can capture hi.research.>.
  ASSERT_TRUE(admin.delete_stream("homeric-research"));
  ASSERT_TRUE(admin.create_conflict_stream());

  NatsClient client(url_);
  ASSERT_TRUE(client.connect());

  // While the conflict holds hi.research.>, js_AddStream("homeric-research")
  // returns NATS_ERR/10065 every attempt; the stream must stay absent.
  EXPECT_FALSE(wait_for([&] { return admin.stream_exists("homeric-research"); }, 5s))
      << "homeric-research appeared despite an overlapping conflict stream";

  // Remove the conflict; the provisioner's backoff retry (cap 2s) must now
  // succeed. Pre-fix code never retries, so this wait fails.
  ASSERT_TRUE(admin.delete_stream(BrokerAdmin::kConflictName));
  EXPECT_TRUE(wait_for([&] { return admin.stream_exists("homeric-research"); }, 30s))
      << "provisioner did not retry and recreate homeric-research after the "
         "conflict was removed — NATS_ERR misclassified as 'stream exists'";

  client.close();
}

// ─── Broker-bounce tests (declaration order matters: these run last) ─────────

// Covers on_disconnected()/on_reconnected() (library-level DisconnectedCB /
// ReconnectedCB) and the reconnect-triggered re-provisioning pass. Uses
// stop+start rather than restart so the disconnected window is deterministic.
TEST_F(NatsClientLiveTest, DisconnectReconnectCallbacksFire) {
  require_compose_ctl();
  NatsClient client(url_);
  ASSERT_TRUE(client.connect());
  const std::string subject = "hi.research.live.bounce-" + unique_suffix();
  ASSERT_TRUE(wait_provisioned(client, subject));

  {
    BrokerRestoreGuard guard;
    ASSERT_TRUE(compose_ctl("stop"));
    EXPECT_TRUE(wait_for([&] { return !client.is_connected(); }, 15s))
        << "DisconnectedCB never fired after broker stop";
  }  // guard restarts the broker here

  EXPECT_TRUE(wait_for([&] { return client.is_connected(); }, 60s))
      << "ReconnectedCB never fired after broker start";
  EXPECT_TRUE(wait_for([&] { return client.publish(subject, R"({"after":"reconnect"})"); }, 30s))
      << "publish did not recover after reconnect (re-provisioning failed)";
  client.close();
}

// Covers the external reconnect_loop() successful-termination path: initial
// connect fails while the broker is down, then the background retry loop
// connects once the broker returns.
TEST_F(NatsClientLiveTest, ReconnectLoopConnectsWhenBrokerReturns) {
  require_compose_ctl();
  BrokerRestoreGuard guard;
  ASSERT_TRUE(compose_ctl("stop"));

  NatsClient client(url_);
  EXPECT_FALSE(client.connect()) << "connect unexpectedly succeeded against a stopped broker";
  EXPECT_FALSE(client.is_connected());

  ASSERT_TRUE(compose_ctl("start"));
  EXPECT_TRUE(wait_for([&] { return client.is_connected(); }, 60s))
      << "background reconnect loop never connected after broker returned";
  client.close();
}

}  // namespace
}  // namespace nestor::test
