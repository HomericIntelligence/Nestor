#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
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
  static constexpr long kDefaultPendingTtlSeconds = 86400;  // 24 hours

  // Construct a store with bounded capacity and TTL-based pending eviction.
  // max_items: hard cap on live entries; submit_research returns {"error":"capacity"} when full.
  // pending_ttl: pending items older than this are swept at the start of the next
  //              submit_research call (lazy eviction — not autonomous).
  explicit Store(std::size_t max_items = kDefaultMaxItems,
                 std::chrono::seconds pending_ttl = std::chrono::seconds{
                     kDefaultPendingTtlSeconds});

  json get_stats() const;

  // Submit a new research task.
  // Returns {"id", ..., "status", "pending"} on success.
  // Returns {"error": "capacity"} when the store is at max_items capacity.
  // A TTL sweep of expired pending items is performed before checking capacity.
  json submit_research(const json& body, const std::string& trace_id = "");

  // Mark a research task as completed.  Merges allow-listed metadata keys
  // (`summary`, `results`, `references`) into the stored item. Reserved keys
  // (`id`, `status`, `submitted_at`, `completed_at`, `idea`, `context`) are
  // never overwritten because they are not in the allow-list. Unknown keys are
  // ignored.
  // Returns the updated item (with status=="completed") on success.
  // Returns {"error": "not_found"} if the id is unknown, was already
  // completed/erased, or was evicted during a prior submit_research sweep.
  // NOTE: this method does NOT trigger TTL eviction. Pending-item TTL sweep
  // runs only in submit_research, so a pending item at/past TTL can still be
  // completed — the caller's intent is honoured.
  json complete_research(const std::string& id, const json& metadata = json::object());

  // Look up a research item by id. Returns the stored item, or
  // {"error": "not_found"} if the id is unknown or has already been completed.
  json get_research(const std::string& id) const;

  // Return all stored research items as {"items": [...], "count": N}.
  json list_research() const;

 private:
  // Struct pairing each research item's JSON with its submission timestamp.
  struct Entry {
    json item;
    std::chrono::system_clock::time_point submitted_at;
  };

  // Evict all pending entries whose age exceeds pending_ttl_.
  // Precondition: caller holds mutex_.
  void evict_expired_pending_locked(std::chrono::system_clock::time_point now);

  mutable std::mutex mutex_;
  // No "active" counter: the current API has only submit_research (→ pending)
  // and complete_research (→ completed) transitions, no claim/start step.
  int completed_{0};
  int pending_{0};
  std::unordered_map<std::string, Entry> research_items_;
  const std::size_t max_items_;
  const std::chrono::seconds pending_ttl_;
  std::atomic<int> expired_{0};
};
}  // namespace projectnestor
