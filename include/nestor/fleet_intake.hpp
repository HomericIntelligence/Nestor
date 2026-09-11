#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace nestor {
using json = nlohmann::json;

class IntakeError : public std::runtime_error {
 public:
  IntakeError(std::string code, int status) : std::runtime_error(std::move(code)), status(status) {}
  const int status;
};

struct IntakeRecord {
  json document;
  std::string sha;
};

// GitHub owns records. A failed or uncertain mutation must throw; no local
// implementation can grant production admission.
class IntakeRepository {
 public:
  virtual ~IntakeRepository() = default;
  virtual std::optional<IntakeRecord> read(const std::string& id) = 0;
  virtual IntakeRecord replace(const std::string& id, const std::string& expected_sha,
                               const json& document) = 0;
  virtual std::vector<json> issues(const std::string& repo) = 0;
  virtual json create_issue(const std::string& repo, const std::string& title,
                            const std::string& body) = 0;
};

class FleetIntake {
 public:
  explicit FleetIntake(std::shared_ptr<IntakeRepository> repository);
  json submit(const json& request);
  json get(const std::string& id);

 private:
  std::shared_ptr<IntakeRepository> repository_;
};
}  // namespace nestor
