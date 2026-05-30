#pragma once

#include "projectnestor/auth_middleware.hpp"
#include "projectnestor/nats_client.hpp"
#include "projectnestor/rate_limiter.hpp"
#include "projectnestor/store.hpp"

#include "httplib.h"

namespace projectnestor {

/// Register all HTTP route handlers onto the server.
/// Issues #40/#44: auth and rate-limiter are required parameters.
/// Call server_main to construct AuthMiddleware and RateLimiter before
/// calling register_routes.
void register_routes(httplib::Server& server, Store& store, NatsClient& nats, AuthMiddleware& auth,
                     RateLimiter& rate);

}  // namespace projectnestor
