#pragma once
#include <cstddef>
#include <deque>
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
  static constexpr std::size_t kDefaultMaxItems = 10'000;

  explicit Store(std::size_t max_items = kDefaultMaxItems);

  json get_stats() const;
  json submit_research(const json& body);

  // Retrieve a single research item by ID. Returns the item JSON, or a JSON
  // object with {"error": "not_found"} if the id is unknown.
  // Issue #64: adds GET /v1/research/:id endpoint.
  [[nodiscard]] json get(const std::string& id) const;

  // List research items with pagination.
  // Returns {"items": [...], "total": N, "offset": O, "limit": L}.
  // Issue #64: adds GET /v1/research endpoint.
  [[nodiscard]] json list(std::size_t offset, std::size_t limit) const;

  // Mark a research task as completed.  Returns the updated item, or a JSON
  // object with {"error": "not_found"} if the id is unknown.
  json complete_research(const std::string& id);

 private:
  // Evict the oldest item from the store. Precondition: caller holds mutex_,
  // insertion_order_ is non-empty, and its front id is present in research_items_.
  void evict_oldest_locked();

  mutable std::mutex mutex_;
  // No "active" counter: the current API has only `submit_research` (→ pending)
  // and `complete_research` (→ completed) transitions, no claim/start step.
  // A permanently-zero `active` field misled operators reading /v1/research/stats.
  // Re-add it once a real "in-progress" state transition exists.
  int completed_{0};
  int pending_{0};
  std::unordered_map<std::string, json> research_items_;
  std::deque<std::string> insertion_order_;
  const std::size_t max_items_;
};
}  // namespace projectnestor
