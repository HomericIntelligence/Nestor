// Nestor store — in-memory research task state with UUID generation.

#include "nestor/store.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>

namespace nestor {

Store::Store(std::size_t max_items, std::chrono::seconds pending_ttl)
    : max_items_(max_items), pending_ttl_(pending_ttl) {}

namespace detail {

std::string generate_uuid() {
  // Seeded once per thread with 256 bits of entropy. A fresh generator seeded
  // per call from a single 32-bit random_device draw collides (identical seed
  // → identical UUID) with probability ~n²/2³³ over n calls — enough to hit
  // duplicate IDs in the 8000-submission concurrency tests on CI.
  thread_local std::mt19937_64 gen = [] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937_64{seq};
  }();
  std::uniform_int_distribution<uint64_t> dis;

  const uint64_t hi = dis(gen);
  const uint64_t lo = dis(gen);

  // Build a UUID v4 string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  // Set version bits (4) and variant bits (10xx)
  const uint64_t hi_v4 = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
  const uint64_t lo_var = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  oss << std::setw(8) << ((hi_v4 >> 32) & 0xFFFFFFFF);
  oss << '-';
  oss << std::setw(4) << ((hi_v4 >> 16) & 0xFFFF);
  oss << '-';
  oss << std::setw(4) << (hi_v4 & 0xFFFF);
  oss << '-';
  oss << std::setw(4) << ((lo_var >> 48) & 0xFFFF);
  oss << '-';
  oss << std::setw(12) << (lo_var & 0xFFFFFFFFFFFFull);
  return oss.str();
}

std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

}  // namespace detail

json Store::get_stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // "active" is intentionally omitted: there is no claim/start state
  // transition in the current API, so reporting it as a permanently-zero
  // value misled operators. Add it back once submit→active→completed exists.
  return json{
      {"completed", completed_},
      {"pending", pending_},
      {"expired", expired_.load()},
  };
}

void Store::evict_expired_pending_locked(std::chrono::system_clock::time_point now) {
  // After the eager-erase change to complete_research, only pending items
  // remain in the map (completed items are removed immediately on transition).
  // The timestamp predicate alone is therefore correct — no status check needed.
  for (auto it = research_items_.begin(); it != research_items_.end();) {
    if (now - it->second.submitted_at >= pending_ttl_) {
      --pending_;
      ++expired_;
      it = research_items_.erase(it);
    } else {
      ++it;
    }
  }
}

json Store::submit_research(const json& body, const std::string& trace_id) {
  const auto now = std::chrono::system_clock::now();
  const std::string id = detail::generate_uuid();
  const std::string submitted_at = detail::now_iso8601();

  const std::string idea = body.value("idea", "");
  const std::string context = body.value("context", "");

  json item = {
      {"id", id},           {"status", "pending"},          {"idea", idea},
      {"context", context}, {"submitted_at", submitted_at}, {"trace_id", trace_id},
  };

  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Sweep TTL-expired pending items before checking capacity.
    // Eviction is lazy: it fires only at insert time, not autonomously.
    // This design means pending items may linger past TTL during idle periods,
    // bounded only by max_items_. Eviction is best-effort, not a hard SLA.
    evict_expired_pending_locked(now);
    if (research_items_.size() >= max_items_) {
      return json{{"error", "capacity"}};
    }
    research_items_[id] = Entry{item, now};
    ++pending_;
  }

  return json{{"id", id}, {"status", "pending"}};
}

json Store::complete_research(const std::string& id, const json& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  // NOTE: no TTL sweep here. Removing the sweep from complete_research
  // eliminates the sweep-before-find race where the item being completed
  // could be silently evicted mid-call. The caller's intent is always
  // honoured: if the item is in the map (even if past TTL), it is completed.
  auto it = research_items_.find(id);
  if (it == research_items_.end()) {
    return json{{"error", "not_found"}};
  }

  // Build result from a local copy before erasing to avoid dangling reference.
  json result = it->second.item;
  result["status"] = "completed";
  result["completed_at"] = detail::now_iso8601();

  // Merge allow-listed metadata keys only. Reserved keys are never overwritten
  // because they are not in the allow-list.
  static constexpr std::array<std::string_view, 3> kAllowed = {"summary", "results", "references"};
  for (const auto key : kAllowed) {
    if (metadata.contains(key)) {
      result[std::string(key)] = metadata[key];
    }
  }

  research_items_.erase(it);
  --pending_;
  ++completed_;

  return result;
}

json Store::get_research(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = research_items_.find(id);
  if (it == research_items_.end()) {
    return json{{"error", "not_found"}};
  }
  return it->second.item;
}

json Store::list_research() const {
  std::lock_guard<std::mutex> lock(mutex_);
  json items = json::array();
  for (const auto& [_, entry] : research_items_) {
    items.push_back(entry.item);
  }
  return json{{"items", items}, {"count", items.size()}};
}

}  // namespace nestor
