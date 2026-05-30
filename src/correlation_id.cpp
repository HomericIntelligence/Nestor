// correlation_id.cpp — X-Correlation-ID tracing helper implementation.
// Issue #49: add correlation ID propagation to HTTP + NATS log payloads.

#include "projectnestor/correlation_id.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace projectnestor {

namespace {

std::string generate_uuid() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  const uint64_t hi = dis(gen);
  const uint64_t lo = dis(gen);

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

}  // namespace

std::string get_or_generate_correlation_id(const httplib::Request& req) {
  const std::string header = req.get_header_value("X-Correlation-ID");
  if (!header.empty()) {
    return header;
  }
  return generate_uuid();
}

void set_correlation_id_header(httplib::Response& res, const std::string& correlation_id) {
  res.set_header("X-Correlation-ID", correlation_id);
}

}  // namespace projectnestor
