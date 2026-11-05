#include "nestor/auth.hpp"
#include "nestor/fleet_intake.hpp"
#include "nestor/routes.hpp"

#include <barrier>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

namespace nestor::test {
class FixtureIntakeRepository final : public IntakeRepository {
 public:
  std::optional<IntakeRecord> read(const std::string&) override {
    std::optional<IntakeRecord> copy;
    {
      std::lock_guard lock(mutex);
      copy = record;
    }
    if (read_barrier) read_barrier->arrive_and_wait();
    return copy;
  }
  IntakeRecord replace(const std::string&, const std::string& expected,
                       const json& document) override {
    std::lock_guard lock(mutex);
    if (fail_write || ++write_attempts == fail_write_number)
      throw IntakeError("github_write_unconfirmed", 503);
    if (expected != (record ? record->sha : "")) {
      ++cas_conflicts;
      throw IntakeError("intake_conflict", 409);
    }
    writes.push_back(document);
    record = IntakeRecord{document, "sha-" + std::to_string(writes.size())};
    return *record;
  }
  std::vector<json> issues(const std::string&) override {
    std::lock_guard lock(mutex);
    return work_issues;
  }
  json create_issue(const std::string& repo, const std::string& title,
                    const std::string& body) override {
    std::lock_guard lock(mutex);
    ++create_calls;
    EXPECT_TRUE(record.has_value());
    EXPECT_EQ(record->document.at("phase"), "creating");
    auto issue = json{
        {"number", 42},
        {"title", title},
        {"body", body},
        {"html_url", "https://github.com/" + (url_repo.empty() ? repo : url_repo) + "/issues/42"}};
    if (create_commits) work_issues.push_back(issue);
    if (lose_create_ack) throw IntakeError("issue_create_unconfirmed", 503);
    return issue;
  }
  std::mutex mutex;
  std::optional<IntakeRecord> record;
  std::vector<json> writes;
  std::vector<json> work_issues;
  int create_calls = 0;
  int write_attempts = 0;
  int fail_write_number = 0;
  int cas_conflicts = 0;
  std::shared_ptr<std::barrier<>> read_barrier;
  std::string url_repo;
  bool fail_write = false;
  bool create_commits = true;
  bool lose_create_ack = false;
};

json intake_request() {
  return {{"schema", "hi/nestor/intake-request/v1"},
          {"intakeId", "reviewed-idea-1"},
          {"workRepository", "Homeric/Work"},
          {"title", "Publishable requirement"},
          {"body", "Publishable requirements belong in this issue."}};
}

TEST(FleetIntake, PersistsCreationIntentBeforeIssueAndRetainsOnlyMetadata) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  FleetIntake intake(repo);
  const auto result = intake.submit(intake_request());
  EXPECT_EQ(result.at("phase"), "created");
  EXPECT_EQ(result.at("issue").at("number"), 42);
  EXPECT_EQ(result.at("workRepository"), "homeric/work");
  ASSERT_EQ(repo->writes.size(), 3u);
  EXPECT_EQ(repo->writes[0].at("phase"), "prepared");
  EXPECT_EQ(repo->writes[1].at("phase"), "creating");
  for (const auto& record : repo->writes) {
    EXPECT_EQ(record.dump().find("Publishable"), std::string::npos);
    EXPECT_FALSE(record.contains("title"));
    EXPECT_FALSE(record.contains("body"));
  }
  EXPECT_EQ(intake.get("reviewed-idea-1"), result);
  EXPECT_EQ(intake.submit(intake_request()), result);
  EXPECT_EQ(repo->create_calls, 1);
}

TEST(FleetIntake, LostIssueAcknowledgmentReconcilesAfterRestartWithoutAnotherCreate) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  repo->lose_create_ack = true;
  FleetIntake first(repo);
  EXPECT_THROW(first.submit(intake_request()), IntakeError);
  ASSERT_TRUE(repo->record);
  ASSERT_EQ(repo->record->document.at("phase"), "creating");
  FleetIntake restarted(repo);
  const auto result = restarted.submit(intake_request());
  EXPECT_EQ(result.at("phase"), "created");
  EXPECT_EQ(repo->create_calls, 1);
}

TEST(FleetIntake, UncertainAttemptWithoutIssueCannotAuthorizeAnotherCreate) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  repo->create_commits = false;
  repo->lose_create_ack = true;
  FleetIntake intake(repo);
  EXPECT_THROW(intake.submit(intake_request()), IntakeError);
  EXPECT_THROW(intake.submit(intake_request()), IntakeError);
  EXPECT_EQ(repo->create_calls, 1);
}

TEST(FleetIntake, FailedDurableWriteCannotCreateIssue) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  repo->fail_write = true;
  FleetIntake intake(repo);
  EXPECT_THROW(intake.submit(intake_request()), IntakeError);
  EXPECT_EQ(repo->create_calls, 0);
}

TEST(FleetIntake, ConcurrentWritersCreateOneIssueAtMost) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  repo->fail_write_number = 2;
  FleetIntake seed(repo);
  EXPECT_THROW(seed.submit(intake_request()), IntakeError);
  ASSERT_TRUE(repo->record);
  ASSERT_EQ(repo->record->document["phase"], "prepared");
  repo->fail_write_number = 0;
  repo->read_barrier = std::make_shared<std::barrier<>>(2);
  auto call = [&] {
    try {
      FleetIntake intake(repo);
      intake.submit(intake_request());
    } catch (const IntakeError&) {
      // A lost compare-and-write or unresolved creation is a rejected admission.
    }
  };
  std::thread first(call), second(call);
  first.join();
  second.join();
  EXPECT_EQ(repo->create_calls, 1);
  EXPECT_EQ(repo->cas_conflicts, 1);
}

TEST(FleetIntake, ChangedRequestCannotReuseIdentity) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  FleetIntake intake(repo);
  intake.submit(intake_request());
  auto changed = intake_request();
  changed["body"] = "Different requirements";
  EXPECT_THROW(intake.submit(changed), IntakeError);
  EXPECT_EQ(repo->create_calls, 1);
}

TEST(FleetIntake, UnconfiguredServiceIsUnavailable) {
  FleetIntake intake(nullptr);
  EXPECT_THROW(intake.submit(intake_request()), IntakeError);
  EXPECT_THROW(intake.get("reviewed-idea-1"), IntakeError);
}

TEST(FleetIntake, GitHubRepositorySpellingDoesNotRejectConfirmedIssue) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  repo->url_repo = "Homeric/Work";
  FleetIntake intake(repo);
  EXPECT_EQ(intake.submit(intake_request())["issue"]["repository"], "homeric/work");
}

TEST(FleetIntake, InvalidSchemaTypeProducesStableValidationFailure) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  FleetIntake intake(repo);
  auto request = intake_request();
  request["schema"] = 12;
  EXPECT_THROW(intake.submit(request), IntakeError);
  EXPECT_EQ(repo->write_attempts, 0);
}

TEST(FleetIntake, CorruptedNestedMetadataCannotLeakOnRead) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  FleetIntake intake(repo);
  intake.submit(intake_request());
  repo->record->document["receipt"]["privateInterview"] = "Do not expose this";
  EXPECT_THROW(intake.get("reviewed-idea-1"), IntakeError);
}

TEST(FleetIntake, LostFinalRecordWriteRetriesThroughIssueReconciliation) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  repo->fail_write_number = 3;
  FleetIntake intake(repo);
  EXPECT_THROW(intake.submit(intake_request()), IntakeError);
  ASSERT_EQ(repo->create_calls, 1);
  repo->fail_write_number = 0;
  FleetIntake restarted(repo);
  EXPECT_EQ(restarted.submit(intake_request())["phase"], "created");
  EXPECT_EQ(repo->create_calls, 1);
}

TEST(FleetIntakeRoutes, UsesAuthenticatedCanonicalIntakeService) {
  auto repo = std::make_shared<FixtureIntakeRepository>();
  FleetIntake intake(repo);
  Store legacy;
  NatsClient nats("nats://127.0.0.1:1");
  RateLimiter limiter(RateLimitConfig{});
  httplib::Server server;
  install_auth_middleware(server, {AuthMode::Required, "fixture-key"});
  register_routes(server, legacy, nats, limiter, &intake);
  const auto port = server.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);
  std::thread thread([&] { server.listen_after_bind(); });
  httplib::Client client("127.0.0.1", port);
  client.set_read_timeout(5);
  const auto unauthorized =
      client.Post("/v1/research/intakes", intake_request().dump(), "application/json");
  EXPECT_TRUE(unauthorized);
  if (unauthorized) EXPECT_EQ(unauthorized->status, 401);
  const httplib::Headers headers{{"Authorization", "Bearer fixture-key"}};
  const auto response =
      client.Post("/v1/research/intakes", headers, intake_request().dump(), "application/json");
  EXPECT_TRUE(response);
  if (response) {
    EXPECT_EQ(response->status, 200);
    if (response->status == 200) EXPECT_EQ(json::parse(response->body)["phase"], "created");
  }
  const auto read = client.Get("/v1/research/intakes/reviewed-idea-1", headers);
  EXPECT_TRUE(read);
  if (read) EXPECT_EQ(read->status, 200);
  EXPECT_EQ(repo->create_calls, 1);
  EXPECT_EQ(legacy.list_research()["count"], 0);
  server.stop();
  thread.join();
}

TEST(FleetIntake, MalformedTypedMetadataIsNeverReturnedAsAnIntake) {
  const std::vector<std::pair<std::string, json>> corruptions = {
      {"workRepository", "private interview text"},
      {"workRepository", "Homeric/Work"},
      {"createdAt", "not-a-date"},
      {"generation", 1.0}};
  for (const auto& [field, value] : corruptions) {
    SCOPED_TRACE(field + ": " + value.dump());
    auto repo = std::make_shared<FixtureIntakeRepository>();
    FleetIntake intake(repo);
    repo->fail_write_number = 2;
    EXPECT_THROW(intake.submit(intake_request()), IntakeError);
    ASSERT_TRUE(repo->record);
    repo->record->document[field] = value;
    EXPECT_THROW(intake.get("reviewed-idea-1"), IntakeError);
    EXPECT_EQ(repo->create_calls, 0);
  }
  auto repo = std::make_shared<FixtureIntakeRepository>();
  FleetIntake intake(repo);
  intake.submit(intake_request());
  repo->record->document["receipt"]["observedAt"] = "private text";
  EXPECT_THROW(intake.get("reviewed-idea-1"), IntakeError);
}

TEST(FleetIntakeRoutes, MissingConfigurationIsExplicitlyUnavailable) {
  Store legacy;
  NatsClient nats("nats://127.0.0.1:1");
  RateLimiter limiter(RateLimitConfig{});
  httplib::Server server;
  register_routes(server, legacy, nats, limiter);
  const auto port = server.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);
  std::thread thread([&] { server.listen_after_bind(); });
  httplib::Client client("127.0.0.1", port);
  client.set_read_timeout(5);
  const auto response =
      client.Post("/v1/research/intakes", intake_request().dump(), "application/json");
  EXPECT_TRUE(response);
  if (response) EXPECT_EQ(response->status, 503);
  EXPECT_EQ(legacy.list_research()["count"], 0);
  server.stop();
  thread.join();
}
}  // namespace nestor::test
