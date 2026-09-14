#include "nestor/fleet_github.hpp"

#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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
    client = std::make_shared<httplib::ClientImpl>("127.0.0.1", port);
    thread = std::thread([this] { server.listen_after_bind(); });
  }
  void TearDown() override {
    server.stop();
    if (thread.joinable()) thread.join();
  }
  httplib::Server server;
  std::shared_ptr<httplib::ClientImpl> client;
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
// A controlled transport boundary: the real pinned serializer receives the
// same retry callback twice. No DNS, TLS failure, or external service is used.
class SerializedStream final : public httplib::Stream {
 public:
  SerializedStream(std::string& written, std::string response)
      : written_(written), response_(std::move(response)) {}
  bool is_readable() const override { return true; }
  bool is_writable() const override { return true; }
  ssize_t read(char* data, size_t size) override {
    const auto count = std::min(size, response_.size() - offset_);
    response_.copy(data, count, offset_);
    offset_ += count;
    return static_cast<ssize_t>(count);
  }
  ssize_t write(const char* data, size_t size) override {
    written_.append(data, size);
    return static_cast<ssize_t>(size);
  }
  void get_remote_ip_and_port(std::string& ip, int& port) const override {
    ip = "127.0.0.1";
    port = 1;
  }
  void get_local_ip_and_port(std::string& ip, int& port) const override {
    ip = "127.0.0.1";
    port = 2;
  }
  socket_t socket() const override { return INVALID_SOCKET; }

 private:
  std::string& written_;
  std::string response_;
  size_t offset_ = 0;
};

class SerializerRetryClient final : public httplib::ClientImpl {
 public:
  SerializerRetryClient() : ClientImpl("fixture.invalid", 443) {}
  ~SerializerRetryClient() override {
    for (const auto peer : peers_) close(peer);
  }
  std::vector<std::string> transmissions;
  bool retry_after_lost_response = true;

 protected:
  bool create_and_connect_socket(Socket& socket, httplib::Error& error) override {
    std::array<int, 2> pair{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0) {
      error = httplib::Error::Connection;
      return false;
    }
    socket.sock = pair[0];
    peers_.push_back(pair[1]);
    return true;
  }

 private:
  bool process_socket(const Socket&, std::function<bool(httplib::Stream&)> callback) override {
    const std::string response =
        "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    transmissions.emplace_back();
    SerializedStream first(transmissions.back(), retry_after_lost_response ? "" : response);
    const auto result = callback(first);
    if (!retry_after_lost_response) return result;
    EXPECT_FALSE(result) << "The fixture must lose the first response after serialization";
    transmissions.emplace_back();
    SerializedStream second(transmissions.back(), response);
    return callback(second);
  }
  std::vector<int> peers_;
};

class FleetGitHubReplay : public ::testing::TestWithParam<std::string> {};

TEST_P(FleetGitHubReplay, LibraryRetryCannotTransmitSecondMutation) {
  auto client = std::make_shared<SerializerRetryClient>();
  const auto request = github_intake_request("fixture-credential", client);
  EXPECT_THROW(request(GetParam(), "/repos/homeric/work/issues", {{"title", "Publishable"}}),
               IntakeError);
  ASSERT_EQ(client->transmissions.size(), 2u);
  EXPECT_TRUE(client->transmissions[0].starts_with(GetParam() + " /repos/homeric/work/issues "));
  EXPECT_NE(client->transmissions[0].find("Authorization: Bearer fixture-credential\r\n"),
            std::string::npos);
  EXPECT_TRUE(client->transmissions[0].ends_with("{\"title\":\"Publishable\"}"));
  EXPECT_TRUE(client->transmissions[1].empty()) << "No retried request bytes may leave the buffer";
  EXPECT_EQ(client->is_socket_open(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Mutations, FleetGitHubReplay, ::testing::Values("POST", "PUT"));

TEST(FleetGitHubTransportContract, ClientCanSendIndependentRequestAfterReplayException) {
  auto client = std::make_shared<SerializerRetryClient>();
  const auto request = github_intake_request("fixture-credential", client);
  EXPECT_THROW(request("POST", "/repos/homeric/work/issues", {{"title", "First"}}), IntakeError);
  EXPECT_EQ(client->is_socket_open(), 0u);
  client->retry_after_lost_response = false;
  EXPECT_EQ(request("GET", "/repos/private/state", nullptr).status, 201);
  ASSERT_EQ(client->transmissions.size(), 3u);
  EXPECT_TRUE(client->transmissions[1].empty());
  EXPECT_TRUE(client->transmissions[2].starts_with("GET /repos/private/state "));
  EXPECT_EQ(client->is_socket_open(), 0u);
}

class SerializedIntakeRepository final : public IntakeRepository {
 public:
  explicit SerializedIntakeRepository(std::shared_ptr<SerializerRetryClient> client)
      : client_(std::move(client)),
        github_(config(), github_intake_request("fixture-credential", client_)) {}
  std::optional<IntakeRecord> read(const std::string&) override { return record; }
  IntakeRecord replace(const std::string&, const std::string& expected,
                       const json& document) override {
    if (expected != (record ? record->sha : "")) throw IntakeError("fixture_conflict", 409);
    record = IntakeRecord{document, std::to_string(++writes_)};
    return *record;
  }
  std::vector<json> issues(const std::string& repo) override {
    std::vector<json> result;
    for (const auto& transmission : client_->transmissions) {
      if (transmission.empty()) continue;
      const auto separator = transmission.find("\r\n\r\n");
      if (separator == std::string::npos) throw IntakeError("fixture_incomplete_request", 503);
      auto issue = json::parse(transmission.substr(separator + 4));
      issue["number"] = 42;
      issue["html_url"] = "https://github.com/" + repo + "/issues/42";
      result.push_back(std::move(issue));
    }
    return result;
  }
  json create_issue(const std::string& repo, const std::string& title,
                    const std::string& body) override {
    EXPECT_TRUE(record.has_value());
    if (!record || record->document["phase"] != "creating")
      throw IntakeError("fixture_missing_creation_intent", 503);
    return github_.create_issue(repo, title, body);
  }
  std::optional<IntakeRecord> record;

 private:
  std::shared_ptr<SerializerRetryClient> client_;
  GitHubIntakeRepository github_;
  int writes_ = 0;
};

TEST(FleetGitHubTransportContract, UncertainTransmissionRetainsIntentAndReconcilesWithoutReplay) {
  auto client = std::make_shared<SerializerRetryClient>();
  auto repository = std::make_shared<SerializedIntakeRepository>(client);
  const json input{{"schema", "hi/nestor/intake-request/v1"},
                   {"intakeId", "reviewed-idea-1"},
                   {"workRepository", "homeric/work"},
                   {"title", "Publishable requirement"},
                   {"body", "Publishable content"}};
  FleetIntake first(repository);
  EXPECT_THROW(first.submit(input), IntakeError);
  ASSERT_TRUE(repository->record);
  EXPECT_EQ(repository->record->document["phase"], "creating");
  ASSERT_EQ(client->transmissions.size(), 2u);
  EXPECT_TRUE(client->transmissions[1].empty());
  FleetIntake restarted(repository);
  const auto reconciled = restarted.submit(input);
  EXPECT_EQ(reconciled["phase"], "created");
  EXPECT_EQ(reconciled["issue"]["number"], 42);
  EXPECT_EQ(client->transmissions.size(), 2u);
  EXPECT_EQ(restarted.submit(input), reconciled);
  EXPECT_EQ(client->transmissions.size(), 2u);
}

}  // namespace nestor::test
