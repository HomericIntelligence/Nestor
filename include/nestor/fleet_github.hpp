#pragma once

#include "nestor/fleet_intake.hpp"

#include <functional>

namespace httplib {
class ClientImpl;
}

namespace nestor {
struct GitHubIntakeConfig {
  std::string state_repository;
  std::string state_branch;
};

struct IntakeHttpResponse {
  int status;
  json body;
};

using IntakeHttpRequest =
    std::function<IntakeHttpResponse(const std::string&, const std::string&, const json&)>;

// Production uses verified HTTPS to api.github.com. A supplied client is a
// trusted transport seam for controlled local tests, never an HTTP API input.
IntakeHttpRequest github_intake_request(const std::string& token,
                                        std::shared_ptr<httplib::ClientImpl> client = nullptr);
std::unique_ptr<FleetIntake> configure_fleet_intake(const GitHubIntakeConfig& config,
                                                    const std::string& token,
                                                    bool required_authentication);

// The injected request is the HTTP boundary, not an alternative state store.
class GitHubIntakeRepository final : public IntakeRepository {
 public:
  GitHubIntakeRepository(GitHubIntakeConfig config, IntakeHttpRequest request);
  std::optional<IntakeRecord> read(const std::string& id) override;
  IntakeRecord replace(const std::string& id, const std::string& expected_sha,
                       const json& document) override;
  std::vector<json> issues(const std::string& repo) override;
  json create_issue(const std::string& repo, const std::string& title,
                    const std::string& body) override;

 private:
  void verify_namespace();
  std::string record_path(const std::string& id) const;
  GitHubIntakeConfig config_;
  IntakeHttpRequest request_;
};
}  // namespace nestor
