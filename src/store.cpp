// ProjectNestor store — in-memory research task state with UUID generation.

#include "projectnestor/store.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace projectnestor {

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
  // "active" is intentionally omitted: there is no claim/start state
  // transition in the current API, so reporting it as a permanently-zero
  // value misled operators. Add it back once submit→active→completed exists.
  return json{
      {"completed", completed_.load()},
      {"pending", pending_.load()},
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
  }
  ++pending_;

  return json{{"id", id}, {"status", "pending"}};
}

json Store::complete_research(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = research_items_.find(id);
  if (it == research_items_.end()) {
    return json{{"error", "not_found"}};
  }

  it->second["status"] = "completed";
  it->second["completed_at"] = detail::now_iso8601();
  --pending_;
  ++completed_;

  return it->second;
}

}  // namespace projectnestor
