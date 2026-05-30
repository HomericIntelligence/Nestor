#pragma once
// correlation_id.hpp — X-Correlation-ID request tracing helper.
//
// Issue #49: No distributed tracing or correlation IDs at the HTTP layer.
//
// Reads X-Correlation-ID from the incoming request (generated if absent),
// threads it into NATS log payloads, and echoes it back in the response header.

#include <string>

#include "httplib.h"

namespace projectnestor {

// Return the X-Correlation-ID from the request, or generate a new UUID v4
// if the header is absent or empty.
[[nodiscard]] std::string get_or_generate_correlation_id(const httplib::Request& req);

// Set the X-Correlation-ID response header.
void set_correlation_id_header(httplib::Response& res, const std::string& correlation_id);

}  // namespace projectnestor
