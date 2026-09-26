#include "nestor/fleet_intake.hpp"

#include "nestor/store.hpp"
#include "nestor/trace_context.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <openssl/evp.h>
#include <regex>
#include <set>
#include <sstream>

namespace nestor {
namespace {
void require(bool condition, const char* code, int status = 409) {
  if (!condition) throw IntakeError(code, status);
}

bool identifier(const std::string& value) {
  return std::regex_match(value, std::regex("[a-z0-9][a-z0-9_-]{7,63}"));
}

std::string digest(const std::string& value) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned int size = 0;
  require(EVP_Digest(value.data(), value.size(), bytes.data(), &size, EVP_sha256(), nullptr) == 1,
          "digest_unavailable", 503);
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < size; ++i) result << std::setw(2) << static_cast<int>(bytes[i]);
  return result.str();
}

bool hex_digest(const json& value) {
  return value.is_string() &&
         std::regex_match(value.get<std::string>(), std::regex("[a-f0-9]{64}"));
}

bool timestamp(const json& value) {
  return value.is_string() &&
         std::regex_match(value.get<std::string>(),
                          std::regex("[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])T"
                                     "([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z"));
}

std::string repository_name(std::string value) {
  require(std::regex_match(value, std::regex("[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+")) &&
              value.size() <= 200 && value.find("..") == std::string::npos,
          "invalid_work_repository", 400);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
  });
  return value;
}

void validate_record(const json& record, const std::string& id) {
  require(record.is_object(), "intake_record_invalid");
  const std::set<std::string> fields{"schema",     "intakeId", "workRepository", "requestDigest",
                                     "bodyDigest", "phase",    "generation",     "createdAt",
                                     "attemptId",  "issue",    "receipt"};
  for (const auto& [key, value] : record.items())
    require(fields.contains(key), "intake_record_invalid");
  for (const auto& key : {"schema", "intakeId", "workRepository", "phase", "createdAt"})
    require(record.contains(key) && record[key].is_string() &&
                record[key].get_ref<const std::string&>().size() <= 200,
            "intake_record_invalid");
  require(record.value("schema", "") == "hi/nestor/intake/v1" &&
              record.value("intakeId", "") == id && record.contains("generation") &&
              record["generation"].is_number_integer() && record["generation"] == 1 &&
              hex_digest(record.value("requestDigest", json())) &&
              hex_digest(record.value("bodyDigest", json())) && record.contains("createdAt") &&
              record["createdAt"].is_string() && record.contains("workRepository") &&
              record["workRepository"].is_string(),
          "intake_record_invalid");
  require(timestamp(record["createdAt"]), "intake_record_invalid");
  const auto stored_repo = record["workRepository"].get<std::string>();
  require(std::regex_match(stored_repo, std::regex("[a-z0-9_-]+/[a-z0-9_.-]+")) &&
              stored_repo.find("..") == std::string::npos,
          "intake_record_invalid");
  const auto phase = record.value("phase", "");
  require(phase == "prepared" || phase == "creating" || phase == "created",
          "intake_record_invalid");
  if (phase != "prepared")
    require(
        record.contains("attemptId") && record["attemptId"].is_string() &&
            std::regex_match(record["attemptId"].get<std::string>(), std::regex("[a-f0-9]{32}")),
        "intake_record_invalid");
  else
    require(!record.contains("attemptId"), "intake_record_invalid");
  require((phase == "created") == record.contains("issue") &&
              (phase == "created") == record.contains("receipt"),
          "intake_record_invalid");
  if (phase == "created") {
    const auto& issue = record["issue"];
    const auto& receipt = record["receipt"];
    require(issue.is_object() && issue.size() == 3 &&
                issue.value("repository", json()) == record["workRepository"] &&
                issue.contains("number") && issue["number"].is_number_integer() &&
                issue["number"].get<int64_t>() > 0,
            "intake_record_invalid");
    require(issue.value("url", json()) ==
                "https://github.com/" + record["workRepository"].get<std::string>() + "/issues/" +
                    std::to_string(issue["number"].get<int64_t>()),
            "intake_record_invalid");
    require(receipt.is_object() && receipt.size() == 2 &&
                receipt.value("kind", json()) == "confirmed_issue" &&
                receipt.contains("observedAt") && receipt["observedAt"].is_string() &&
                timestamp(receipt["observedAt"]),
            "intake_record_invalid");
  }
}

json issue_reference(const json& issue, const std::string& repo, const std::string& title,
                     const std::string& body) {
  require(issue.is_object() && !issue.contains("pull_request") && issue.contains("number") &&
              issue["number"].is_number_integer() && issue["number"].get<int64_t>() > 0 &&
              issue.value("title", "") == title && issue.value("body", "") == body,
          "intake_issue_conflict");
  const auto number = issue["number"].get<int64_t>();
  const auto url = "https://github.com/" + repo + "/issues/" + std::to_string(number);
  auto actual_url = issue.value("html_url", "");
  std::transform(actual_url.begin(), actual_url.end(), actual_url.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
  });
  require(actual_url == url, "intake_issue_conflict");
  return {{"repository", repo}, {"number", number}, {"url", url}};
}
}  // namespace

FleetIntake::FleetIntake(std::shared_ptr<IntakeRepository> repository)
    : repository_(std::move(repository)) {}

json FleetIntake::submit(const json& request) {
  require(repository_ != nullptr, "fleet_intake_unavailable", 503);
  require(request.is_object() && request.size() == 5 && request.contains("schema") &&
              request["schema"].is_string() &&
              request.value("schema", "") == "hi/nestor/intake-request/v1" &&
              request.contains("intakeId") && request["intakeId"].is_string() &&
              request.contains("workRepository") && request["workRepository"].is_string() &&
              request.contains("title") && request["title"].is_string() &&
              request.contains("body") && request["body"].is_string(),
          "invalid_intake_request", 400);
  const auto id = request["intakeId"].get<std::string>();
  const auto repo = repository_name(request["workRepository"].get<std::string>());
  const auto title = request["title"].get<std::string>();
  require(identifier(id) && !title.empty() && title.size() <= 256 &&
              request["body"].get_ref<const std::string&>().size() <= 60000,
          "invalid_intake_request", 400);
  auto canonical = request;
  canonical["workRepository"] = repo;
  const auto request_digest = digest(canonical.dump());
  const auto marker = "<!-- nestor:fleet-intake:v1 id=" + id + " digest=" + request_digest + " -->";
  const auto body = request["body"].get<std::string>() + "\n\n" + marker;
  require(request["body"].get_ref<const std::string&>().find("nestor:fleet-intake:") ==
              std::string::npos,
          "reserved_intake_marker", 400);

  auto write = [&](const std::string& sha, const json& document) {
    const auto result = repository_->replace(id, sha, document);
    require(result.document == document && !result.sha.empty(), "intake_write_unconfirmed", 503);
    return result;
  };
  auto record = repository_->read(id);
  if (!record) {
    record = write("", {{"schema", "hi/nestor/intake/v1"},
                        {"intakeId", id},
                        {"workRepository", repo},
                        {"requestDigest", request_digest},
                        {"bodyDigest", digest(body)},
                        {"phase", "prepared"},
                        {"generation", 1},
                        {"createdAt", detail::now_iso8601()}});
  }
  validate_record(record->document, id);
  require(record->document["requestDigest"] == request_digest &&
              record->document["bodyDigest"] == digest(body) &&
              record->document["workRepository"] == repo,
          "intake_request_conflict");
  if (record->document["phase"] == "created") return record->document;

  json issue;
  if (record->document["phase"] == "prepared") {
    auto creating = record->document;
    creating["phase"] = "creating";
    creating["attemptId"] = detail::generate_trace_id();
    record = write(record->sha, creating);
    // No automatic retry is permitted after this boundary. A timeout does not
    // prove that GitHub rejected creation, or that this process has stopped.
    issue = repository_->create_issue(repo, title, body);
  } else {
    std::vector<json> candidates;
    for (const auto& candidate : repository_->issues(repo)) {
      if (candidate.is_object() && candidate.contains("body") && candidate["body"].is_string() &&
          candidate["body"].get_ref<const std::string&>().find(marker) != std::string::npos)
        candidates.push_back(candidate);
    }
    require(candidates.size() == 1, candidates.empty() ? "intake_creation_requires_reconciliation"
                                                       : "intake_duplicate_issues");
    issue = candidates.front();
  }
  const auto reference = issue_reference(issue, repo, title, body);
  auto created = record->document;
  created["phase"] = "created";
  created["issue"] = reference;
  created["receipt"] = {{"kind", "confirmed_issue"}, {"observedAt", detail::now_iso8601()}};
  return write(record->sha, created).document;
}
json FleetIntake::get(const std::string& id) {
  require(repository_ != nullptr, "fleet_intake_unavailable", 503);
  require(identifier(id), "invalid_intake_id", 400);
  const auto record = repository_->read(id);
  require(record.has_value(), "intake_not_found", 404);
  validate_record(record->document, id);
  return record->document;
}
}  // namespace nestor
