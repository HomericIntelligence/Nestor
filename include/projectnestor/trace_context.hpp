#pragma once
#include <string>

#include "httplib.h"

namespace projectnestor {

struct TraceContext {
  std::string trace_id;
  std::string span_id;
};

// Extract trace context from HTTP request or generate fresh IDs.
// Fallback chain: W3C traceparent → X-Request-ID → generate both fresh.
TraceContext extract_or_generate(const httplib::Request& req);

// Encode trace context as W3C traceparent header (format: 00-<trace_id>-<span_id>-01).
std::string to_traceparent_header(const TraceContext& ctx);

namespace detail {

// Parse W3C traceparent header. Returns true if valid and all-zero checks pass.
// On success, trace_id is 32 lowercase hex, span_id is 16 lowercase hex.
// Validates format: 00-<32 hex>-<16 hex>-<2 hex>; rejects all-zero trace/span.
bool parse_traceparent(const std::string& header, std::string& trace_id, std::string& span_id);

// Generate a 32-character lowercase hex trace ID.
std::string generate_trace_id();

// Generate a 16-character lowercase hex span ID.
std::string generate_span_id();

// Check if a string is lowercase hex.
bool is_lower_hex(const std::string& s);

}  // namespace detail

}  // namespace projectnestor
