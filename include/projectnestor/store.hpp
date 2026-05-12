#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

namespace projectnestor {
using json = nlohmann::json;

namespace detail {
// Internal helpers — exposed for unit tests only. Not part of the public API
// and may change without notice. Do NOT call from downstream code.
std::string generate_uuid();
std::string now_iso8601();
}  // namespace detail

class Store {
 public:
  json get_stats() const;
  json submit_research(const json& body);

  // Mark a research task as completed.  Returns the updated item, or a JSON
  // object with {"error": "not_found"} if the id is unknown.
  json complete_research(const std::string& id);

 private:
  mutable std::mutex mutex_;
  // No "active" counter: the current API has only `submit_research` (→ pending)
  // and `complete_research` (→ completed) transitions, no claim/start step.
  // A permanently-zero `active` field misled operators reading /v1/research/stats.
  // Re-add it once a real "in-progress" state transition exists.
  std::atomic<int> completed_{0};
  std::atomic<int> pending_{0};
  std::unordered_map<std::string, json> research_items_;
};
}  // namespace projectnestor
