// ProjectNestor store — in-memory research task state with UUID generation.

#include "projectnestor/store.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace projectnestor {

Store::Store(std::size_t max_items) : max_items_(max_items) {}

namespace detail {

std::string generate_uuid() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
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
      {"items_count", static_cast<int>(research_items_.size())},
  };
}

json Store::submit_research(const json& body) {
  const std::string id = detail::generate_uuid();
  const std::string submitted_at = detail::now_iso8601();

  const std::string idea = body.value("idea", "");
  const std::string context = body.value("context", "");

  json item = {
      {"id", id},           {"status", "pending"},          {"idea", idea},
      {"context", context}, {"submitted_at", submitted_at},
  };

  {
    std::lock_guard<std::mutex> lock(mutex_);
    research_items_[id] = item;
    insertion_order_.push_back(id);
    ++pending_;
    while (research_items_.size() > max_items_) {
      evict_oldest_locked();
    }
  }

  return json{{"id", id}, {"status", "pending"}};
}

json Store::get(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = research_items_.find(id);
  if (it == research_items_.end()) {
    return json{{"error", "not_found"}};
  }
  return it->second;
}

json Store::list(std::size_t offset, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  json items = json::array();
  const std::size_t total = insertion_order_.size();
  const std::size_t end = std::min(offset + limit, total);
  for (std::size_t i = offset; i < end; ++i) {
    auto it = research_items_.find(insertion_order_[i]);
    if (it != research_items_.end()) {
      items.push_back(it->second);
    }
  }
  return json{
      {"items", items},
      {"total", static_cast<int>(total)},
      {"offset", static_cast<int>(offset)},
      {"limit", static_cast<int>(limit)},
  };
}

json Store::complete_research(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = research_items_.find(id);
  if (it == research_items_.end()) {
    return json{{"error", "not_found"}};
  }

  it->second["status"] = "completed";
  it->second["completed_at"] = detail::now_iso8601();
  json result = it->second;

  --pending_;
  ++completed_;

  // Erase from map
  research_items_.erase(it);

  // Erase from insertion order (linear scan, but bounded by max_items_)
  auto pos = std::find(insertion_order_.begin(), insertion_order_.end(), id);
  if (pos != insertion_order_.end()) {
    insertion_order_.erase(pos);
  }

  return result;
}

void Store::evict_oldest_locked() {
  assert(!insertion_order_.empty());
  const std::string id = std::move(insertion_order_.front());
  insertion_order_.pop_front();

  auto it = research_items_.find(id);
  assert(it != research_items_.end());

  const std::string status = it->second.value("status", "");
  if (status == "pending") {
    --pending_;
  } else if (status == "completed") {
    --completed_;
  }

  research_items_.erase(it);
}

}  // namespace projectnestor
