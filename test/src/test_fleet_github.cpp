#include "nestor/fleet_github.hpp"

#include <openssl/evp.h>
#include <thread>

#include "httplib.h"
#include <gtest/gtest.h>

namespace nestor::test {
namespace {
std::string encoded(const std::string& input) {
  std::string result(4 * ((input.size() + 2) / 3) + 1, '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                    reinterpret_cast<const unsigned char*>(input.data()),
                                    static_cast<int>(input.size()));
  result.resize(static_cast<size_t>(size));
  return result;
}
const std::string old_sha(40, 'a'), new_sha(40, 'b');
const std::string path = "nestor/intakes/reviewed-idea-1.json";
GitHubIntakeConfig config() { return {"private/state", "fleet-state"}; }

struct HttpFixture {
  std::vector<json> requests;
  json document = {{"phase", "creating"}};
  bool file_exists = true;
  bool private_repository = true;
  bool branch_exists = true;
  bool commit_put = true;
  bool lose_put_response = false;
  bool lose_issue_response = false;
  int put_status = 200;
  int post_count = 0;
  std::string sha = old_sha;

  IntakeHttpResponse operator()(const std::string& method, const std::string& target,
                                const json& body) {
    requests.push_back({{"method", method}, {"target", target}, {"body", body}});
    if (target == "/repos/private/state") return {200, {{"private", private_repository}}};
    if (target == "/repos/private/state/git/ref/heads/fleet-state")
      return {branch_exists ? 200 : 404, {{"ref", "refs/heads/fleet-state"}}};
    if (method == "GET" && target == "/repos/private/state/contents/" + path + "?ref=fleet-state") {
      if (!file_exists) return {404, json::object()};
      return {200,
              {{"type", "file"},
               {"path", path},
               {"sha", sha},
               {"encoding", "base64"},
               {"content", encoded(document.dump() + "\n")}}};
    }
    if (method == "PUT" && target == "/repos/private/state/contents/" + path) {
      EXPECT_EQ(body.at("branch"), "fleet-state");
      EXPECT_EQ(body.at("content"), encoded(document.dump() + "\n"));
      if (put_status != 200) return {put_status, json::object()};
      if (commit_put) sha = new_sha;
      if (lose_put_response) throw IntakeError("github_transport_unconfirmed", 503);
      return {200,
              {{"content", {{"path", path}, {"sha", new_sha}}}, {"commit", {{"sha", new_sha}}}}};
    }
    if (method == "GET" && target == "/repos/homeric/work/issues?state=all&per_page=100&page=1")
      return {200, json::array({{{"number", 42}, {"state", "closed"}}})};
    if (method == "POST" && target == "/repos/homeric/work/issues") {
      ++post_count;
      if (lose_issue_response) throw IntakeError("github_transport_unconfirmed", 503);
      return {201, {{"number", 42}, {"title", body.at("title")}, {"body", body.at("body")}}};
    }
    throw IntakeError("unexpected_fixture_request", 503);
  }
};
}  // namespace

TEST(FleetGitHub, ReadsExactMetadataAtExplicitPrivateBranch) {
  HttpFixture fixture;
  GitHubIntakeRepository repo(config(), std::ref(fixture));
  const auto record = repo.read("reviewed-idea-1");
  ASSERT_TRUE(record);
  EXPECT_EQ(record->sha, old_sha);
  EXPECT_EQ(record->document, fixture.document);
}

TEST(FleetGitHub, CompareWriteSendsPriorShaAndConfirmsExactReadback) {
  HttpFixture fixture;
  GitHubIntakeRepository repo(config(), std::ref(fixture));
  const auto record = repo.replace("reviewed-idea-1", old_sha, fixture.document);
  EXPECT_EQ(record.sha, new_sha);
  bool observed_put = false;
  for (const auto& request : fixture.requests) {
    if (request["method"] == "PUT") {
      observed_put = true;
      EXPECT_EQ(request["body"]["sha"], old_sha);
    }
  }
  EXPECT_TRUE(observed_put);
}

TEST(FleetGitHub, LostWriteResponseAndConflictsNeverBecomeAdmission) {
  for (const int status : {200, 409, 422}) {
    HttpFixture fixture;
    fixture.put_status = status;
    fixture.lose_put_response = status == 200;
    GitHubIntakeRepository repo(config(), std::ref(fixture));
    EXPECT_THROW(repo.replace("reviewed-idea-1", old_sha, fixture.document), IntakeError);
    EXPECT_EQ(fixture.post_count, 0);
  }
}

TEST(FleetGitHub, UnconfirmedReadbackCannotAuthorizeCreation) {
  HttpFixture fixture;
  fixture.commit_put = false;
  GitHubIntakeRepository repo(config(), std::ref(fixture));
  EXPECT_THROW(repo.replace("reviewed-idea-1", old_sha, fixture.document), IntakeError);
}

TEST(FleetGitHub, MissingRecordRequiresExistingPrivateRepositoryAndBranch) {
  HttpFixture fixture;
  fixture.file_exists = false;
  GitHubIntakeRepository repo(config(), std::ref(fixture));
  EXPECT_FALSE(repo.read("reviewed-idea-1"));
  fixture.private_repository = false;
  EXPECT_THROW(repo.read("reviewed-idea-1"), IntakeError);
  fixture.private_repository = true;
  fixture.branch_exists = false;
  EXPECT_THROW(repo.read("reviewed-idea-1"), IntakeError);
}

TEST(FleetGitHub, ReconciliationIncludesClosedIssues) {
  HttpFixture fixture;
  GitHubIntakeRepository repo(config(), std::ref(fixture));
  const auto issues = repo.issues("homeric/work");
  ASSERT_EQ(issues.size(), 1u);
  EXPECT_EQ(issues.front()["state"], "closed");
}

TEST(FleetGitHub, IssueCreationMakesOneTransportAttempt) {
  HttpFixture fixture;
  fixture.lose_issue_response = true;
  GitHubIntakeRepository repo(config(), std::ref(fixture));
  EXPECT_THROW(repo.create_issue("homeric/work", "Title", "Body"), IntakeError);
  EXPECT_EQ(fixture.post_count, 1);
}

class FleetGitHubTransport : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    client = std::make_shared<httplib::Client>("127.0.0.1", port);
    thread = std::thread([this] { server.listen_after_bind(); });
  }
  void TearDown() override {
    server.stop();
    if (thread.joinable()) thread.join();
  }
  httplib::Server server;
  std::shared_ptr<httplib::Client> client;
  std::thread thread;
};

TEST_F(FleetGitHubTransport, AuthenticatedJsonMutationUsesExactlyOneRequest) {
  int calls = 0;
  server.Post("/repos/homeric/work/issues", [&](const auto& req, auto& res) {
    ++calls;
    EXPECT_EQ(req.get_header_value("Authorization"), "Bearer fixture-credential");
    EXPECT_EQ(req.get_header_value("X-GitHub-Api-Version"), "2022-11-28");
    EXPECT_EQ(json::parse(req.body), (json{{"title", "Publishable"}}));
    res.status = 503;
    res.set_content("{\"message\":\"unconfirmed\"}", "application/json");
  });
  const auto request = github_intake_request("fixture-credential", client);
  const auto result = request("POST", "/repos/homeric/work/issues", {{"title", "Publishable"}});
  EXPECT_EQ(result.status, 503);
  EXPECT_EQ(calls, 1);
}

TEST_F(FleetGitHubTransport, RedirectNeverForwardsAuthorizationOrRepeatsMutation) {
  int followed = 0;
  server.Get("/repos/private/state", [](const auto&, auto& res) {
    res.set_redirect("/redirected", 302);
    res.set_content("{}", "application/json");
  });
  server.Get("/redirected", [&](const auto&, auto& res) {
    ++followed;
    res.set_content("{}", "application/json");
  });
  const auto request = github_intake_request("fixture-credential", client);
  EXPECT_EQ(request("GET", "/repos/private/state", nullptr).status, 302);
  EXPECT_EQ(followed, 0);
}

TEST_F(FleetGitHubTransport, InvalidAndOversizedResponsesFailClosed) {
  server.Get("/repos/invalid",
             [](const auto&, auto& res) { res.set_content("invalid", "text/plain"); });
  server.Get("/repos/large", [](const auto&, auto& res) {
    res.set_content(std::string(8 * 1024 * 1024 + 1, 'x'), "application/json");
  });
  const auto request = github_intake_request("fixture-credential", client);
  EXPECT_THROW(request("GET", "/repos/invalid", nullptr), IntakeError);
  EXPECT_THROW(request("GET", "/repos/large", nullptr), IntakeError);
  EXPECT_THROW(request("DELETE", "/repos/private/state", nullptr), IntakeError);
  EXPECT_THROW(request("GET", "https://untrusted.example/", nullptr), IntakeError);
}

TEST(FleetGitHubConfiguration, ExplicitCompletePrivateConfigurationAndAuthAreRequired) {
  EXPECT_EQ(configure_fleet_intake({}, "", true), nullptr);
  EXPECT_EQ(configure_fleet_intake({}, "unrelated-backend-token", true), nullptr);
  EXPECT_THROW(configure_fleet_intake({"private/state", ""}, "fixture-credential", true),
               IntakeError);
  EXPECT_THROW(configure_fleet_intake(config(), "", true), IntakeError);
  EXPECT_THROW(configure_fleet_intake(config(), "fixture-credential", false), IntakeError);
  EXPECT_THROW(configure_fleet_intake(config(), "invalid\nheader", true), IntakeError);
  EXPECT_NE(configure_fleet_intake(config(), "fixture-credential", true), nullptr);
}
}  // namespace nestor::test
