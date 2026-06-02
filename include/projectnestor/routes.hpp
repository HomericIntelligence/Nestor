#pragma once

#include "projectnestor/nats_client.hpp"
#include "projectnestor/rate_limiter.hpp"
#include "projectnestor/store.hpp"

#include "httplib.h"

namespace projectnestor {

/// Register all HTTP route handlers onto the server.
///
/// @param limiter  Rate limiter constructed on the main() stack; must outlive
///                 server.listen(). Passed by reference; captured as pointer
///                 by route lambdas (see cpp-httplib-lambda-capture-ub skill).
void register_routes(httplib::Server& server, Store& store, NatsClient& nats, RateLimiter& limiter);

}  // namespace projectnestor
