#include "nestor/fleet_github.hpp"

#include <algorithm>
#include <chrono>
#include <openssl/evp.h>
#include <regex>

#include "httplib.h"

namespace nestor {
namespace {
void require(bool condition, const char* code, int status = 503) {
  if (!condition) throw IntakeError(code, status);
}
bool sha_value(const json& value) {
  return value.is_string() &&
         std::regex_match(value.get<std::string>(), std::regex("[0-9a-f]{40}|[0-9a-f]{64}"));
}
void repository_name(const std::string& repo) {
  require(repo.size() <= 200 && repo.find("..") == std::string::npos &&
              std::regex_match(repo, std::regex("[a-z0-9_-]+/[a-z0-9_.-]+")),
          "github_repository_invalid", 400);
}
std::string encode(const std::string& input) {
  std::string result(4 * ((input.size() + 2) / 3) + 1, '\0');
  const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                   reinterpret_cast<const unsigned char*>(input.data()),
                                   static_cast<int>(input.size()));
  require(size >= 0, "github_encoding_failed");
  result.resize(static_cast<size_t>(size));
  return result;
}
std::string decode(std::string input) {
  input.erase(std::remove(input.begin(), input.end(), '\n'), input.end());
  input.erase(std::remove(input.begin(), input.end(), '\r'), input.end());
  require(!input.empty() && input.size() <= 32768 && input.size() % 4 == 0 &&
              std::regex_match(input, std::regex("[A-Za-z0-9+/]*={0,2}")),
          "github_record_encoding_invalid");
  std::string result((input.size() / 4) * 3, '\0');
  int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                             reinterpret_cast<const unsigned char*>(input.data()),
                             static_cast<int>(input.size()));
  require(size >= 0, "github_record_encoding_invalid");
  if (input.back() == '=') --size;
  if (input[input.size() - 2] == '=') --size;
  result.resize(static_cast<size_t>(size));
  require(encode(result) == input, "github_record_encoding_invalid");
  return result;
}
}  // namespace

IntakeHttpRequest github_intake_request(const std::string& token,
                                        std::shared_ptr<httplib::Client> client) {
  require(!token.empty() && token.size() <= 256 &&
              std::all_of(token.begin(), token.end(),
                          [](unsigned char value) { return value > 32 && value < 127; }),
          "github_backend_token_invalid", 503);
  if (!client) client = std::make_shared<httplib::Client>("https://api.github.com");
  client->set_follow_location(false);
  client->enable_server_certificate_verification(true);
  client->set_connection_timeout(3);
  client->set_read_timeout(5);
  client->set_write_timeout(5);
  client->set_keep_alive(false);
  return [client, token](const std::string& method, const std::string& path,
                         const json& body) -> IntakeHttpResponse {
    require((method == "GET" || method == "PUT" || method == "POST") &&
                path.starts_with("/repos/") && path.size() <= 512 &&
                std::all_of(path.begin(), path.end(),
                            [](unsigned char value) { return value > 32 && value < 127; }),
            "github_request_invalid", 400);
    httplib::Request request;
    request.method = method;
    request.path = path;
    request.headers = {{"Authorization", "Bearer " + token},
                       {"Accept", "application/vnd.github+json"},
                       {"X-GitHub-Api-Version", "2022-11-28"},
                       {"User-Agent", "Nestor-Fleet"}};
    if (!body.is_null()) {
      request.body = body.dump();
      require(request.body.size() <= 1024 * 1024, "github_request_invalid", 400);
      request.set_header("Content-Type", "application/json");
    }
    constexpr std::size_t limit = 8 * 1024 * 1024;
    std::string bytes;
    request.content_receiver = [&bytes](const char* data, size_t size, uint64_t, uint64_t) {
      if (size > limit - bytes.size()) return false;
      bytes.append(data, size);
      return true;
    };
    // A single send is deliberate: no retry, redirect, proxy environment,
    // authentication refresh, or alternate endpoint may repeat a mutation.
    const auto result = client->send(request);
    require(static_cast<bool>(result), "github_transport_unconfirmed");
    auto parsed = json::parse(bytes, nullptr, false);
    require(!parsed.is_discarded(), "github_response_invalid");
    return {result->status, std::move(parsed)};
  };
}

std::unique_ptr<FleetIntake> configure_fleet_intake(const GitHubIntakeConfig& config,
                                                    const std::string& token,
                                                    bool required_authentication) {
  if (config.state_repository.empty() && config.state_branch.empty()) return nullptr;
  require(
      required_authentication && !config.state_repository.empty() && !config.state_branch.empty(),
      "fleet_intake_configuration_invalid");
  return std::make_unique<FleetIntake>(
      std::make_shared<GitHubIntakeRepository>(config, github_intake_request(token)));
}

GitHubIntakeRepository::GitHubIntakeRepository(GitHubIntakeConfig config, IntakeHttpRequest request)
    : config_(std::move(config)), request_(std::move(request)) {
  repository_name(config_.state_repository);
  require(
      request_ && config_.state_branch.find("..") == std::string::npos &&
          std::regex_match(config_.state_branch, std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")),
      "github_intake_configuration_invalid", 400);
}

void GitHubIntakeRepository::verify_namespace() {
  const auto repository = request_("GET", "/repos/" + config_.state_repository, nullptr);
  require(repository.status == 200 && repository.body.is_object() &&
              repository.body.value("private", json()) == true,
          "github_private_state_repository_required");
  const auto branch = request_(
      "GET", "/repos/" + config_.state_repository + "/git/ref/heads/" + config_.state_branch,
      nullptr);
  require(branch.status == 200 && branch.body.is_object() &&
              branch.body.value("ref", json()) == "refs/heads/" + config_.state_branch,
          "github_state_branch_unavailable");
}

std::string GitHubIntakeRepository::record_path(const std::string& id) const {
  require(std::regex_match(id, std::regex("[a-z0-9][a-z0-9_-]{7,63}")), "invalid_intake_id", 400);
  return "nestor/intakes/" + id + ".json";
}

std::optional<IntakeRecord> GitHubIntakeRepository::read(const std::string& id) {
  const auto path = record_path(id);
  verify_namespace();
  const auto response = request_(
      "GET",
      "/repos/" + config_.state_repository + "/contents/" + path + "?ref=" + config_.state_branch,
      nullptr);
  if (response.status == 404) return std::nullopt;
  const auto& body = response.body;
  require(response.status == 200 && body.is_object() && body.value("type", json()) == "file" &&
              body.value("path", json()) == path && body.value("encoding", json()) == "base64" &&
              sha_value(body.value("sha", json())) && body.contains("content") &&
              body["content"].is_string(),
          "github_record_read_unconfirmed");
  const auto document = json::parse(decode(body["content"].get<std::string>()), nullptr, false);
  require(document.is_object(), "github_record_invalid");
  return IntakeRecord{document, body["sha"].get<std::string>()};
}

IntakeRecord GitHubIntakeRepository::replace(const std::string& id, const std::string& expected_sha,
                                             const json& document) {
  const auto path = record_path(id);
  const auto bytes = document.dump() + "\n";
  require(document.is_object() && bytes.size() <= 16384 &&
              (expected_sha.empty() || sha_value(expected_sha)),
          "github_record_write_invalid", 400);
  verify_namespace();
  json request = {{"message", "Record Nestor intake " + id},
                  {"content", encode(bytes)},
                  {"branch", config_.state_branch}};
  if (!expected_sha.empty()) request["sha"] = expected_sha;
  // One attempt only. Even a successful read after a lost response cannot
  // authorize this invocation to repeat a possibly committed operation.
  const auto response =
      request_("PUT", "/repos/" + config_.state_repository + "/contents/" + path, request);
  if (response.status == 409 || response.status == 422)
    throw IntakeError("github_compare_write_conflict", 409);
  const auto& body = response.body;
  require((response.status == 200 || response.status == 201) && body.is_object() &&
              body.contains("content") && body["content"].is_object() &&
              body["content"].value("path", json()) == path &&
              sha_value(body["content"].value("sha", json())) && body.contains("commit") &&
              body["commit"].is_object() && sha_value(body["commit"].value("sha", json())),
          "github_record_write_unconfirmed");
  const auto confirmed = read(id);
  require(confirmed && confirmed->sha == body["content"]["sha"].get<std::string>() &&
              confirmed->document == document,
          "github_record_write_unconfirmed");
  return *confirmed;
}

std::vector<json> GitHubIntakeRepository::issues(const std::string& repo) {
  repository_name(repo);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::vector<json> result;
  size_t bytes = 0;
  for (int page = 1; page <= 100; ++page) {
    require(std::chrono::steady_clock::now() < deadline, "github_issue_enumeration_incomplete");
    const auto response = request_(
        "GET", "/repos/" + repo + "/issues?state=all&per_page=100&page=" + std::to_string(page),
        nullptr);
    require(response.status == 200 && response.body.is_array() && response.body.size() <= 100 &&
                std::chrono::steady_clock::now() < deadline,
            "github_issue_enumeration_incomplete");
    bytes += response.body.dump().size();
    require(bytes <= 16 * 1024 * 1024, "github_issue_enumeration_incomplete");
    for (const auto& issue : response.body) result.push_back(issue);
    if (response.body.size() < 100) return result;
  }
  throw IntakeError("github_issue_enumeration_incomplete", 503);
}

json GitHubIntakeRepository::create_issue(const std::string& repo, const std::string& title,
                                          const std::string& body) {
  repository_name(repo);
  const auto response =
      request_("POST", "/repos/" + repo + "/issues", {{"title", title}, {"body", body}});
  require(response.status == 201 && response.body.is_object(), "github_issue_create_unconfirmed");
  return response.body;
}
}  // namespace nestor
