// Nestor trace context — W3C traceparent parsing and correlation ID generation.

#include "nestor/trace_context.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace nestor {

namespace detail {

bool is_lower_hex(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string generate_trace_id() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  const uint64_t hi = dis(gen);
  const uint64_t lo = dis(gen);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  oss << std::setw(16) << hi;
  oss << std::setw(16) << lo;
  return oss.str();
}

std::string generate_span_id() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  const uint64_t val = dis(gen);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  oss << std::setw(16) << val;
  return oss.str();
}

bool parse_traceparent(const std::string& header, std::string& trace_id, std::string& span_id) {
  // Format: 00-<32 hex>-<16 hex>-<2 hex>
  // Total: 2 + 1 + 32 + 1 + 16 + 1 + 2 = 55 characters
  if (header.length() != 55) {
    return false;
  }

  // Check version (first two chars must be "00")
  if (header[0] != '0' || header[1] != '0') {
    return false;
  }

  // Check separators
  if (header[2] != '-' || header[35] != '-' || header[52] != '-') {
    return false;
  }

  trace_id = header.substr(3, 32);
  span_id = header.substr(36, 16);
  const std::string flags = header.substr(53, 2);

  // Validate trace_id and span_id are lowercase hex
  if (!is_lower_hex(trace_id) || !is_lower_hex(span_id)) {
    return false;
  }

  // Validate flags are hex (not necessarily lowercase)
  for (char c : flags) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }

  // Reject all-zero trace_id and all-zero span_id
  if (trace_id == std::string(32, '0') || span_id == std::string(16, '0')) {
    return false;
  }

  return true;
}

}  // namespace detail

std::string normalize_x_request_id(const std::string& header) {
  // Remove hyphens and lowercase the string
  std::string normalized;
  for (char c : header) {
    if (c == '-') {
      continue;
    }
    if (c >= 'A' && c <= 'Z') {
      normalized += static_cast<char>(c - 'A' + 'a');
    } else {
      normalized += c;
    }
  }
  return normalized;
}

TraceContext extract_or_generate(const httplib::Request& req) {
  // Priority 1: W3C traceparent header
  const std::string traceparent = req.get_header_value("traceparent");
  if (!traceparent.empty()) {
    std::string trace_id, span_id;
    if (detail::parse_traceparent(traceparent, trace_id, span_id)) {
      return {trace_id, span_id};
    }
  }

  // Priority 2: X-Request-ID header (normalize to 32 hex, generate fresh span)
  const std::string x_request_id = req.get_header_value("X-Request-ID");
  if (!x_request_id.empty()) {
    const std::string normalized = normalize_x_request_id(x_request_id);
    if (normalized.length() == 32 && detail::is_lower_hex(normalized)) {
      return {normalized, detail::generate_span_id()};
    }
  }

  // Priority 3: Generate fresh trace_id and span_id
  return {detail::generate_trace_id(), detail::generate_span_id()};
}

std::string to_traceparent_header(const TraceContext& ctx) {
  std::ostringstream oss;
  oss << "00-" << ctx.trace_id << "-" << ctx.span_id << "-01";
  return oss.str();
}

}  // namespace nestor
