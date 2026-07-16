#pragma once

#include "nestor/nats_client.hpp"
#include "nestor/rate_limiter.hpp"
#include "nestor/store.hpp"

#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace nestor {

/// Register all HTTP route handlers onto the server.
///
/// @param limiter  Rate limiter constructed on the main() stack; must outlive
///                 server.listen(). Passed by reference; captured as pointer
///                 by route lambdas (see cpp-httplib-lambda-capture-ub skill).
void register_routes(httplib::Server& server, Store& store, NatsClient& nats, RateLimiter& limiter);

// ── HMAS mesh wire helpers (Odysseus ADR-013 §7) ─────────────────────────────
// Pure functions, exposed for unit testing.

/// Role-addressed research-pool dispatch subject for a research id:
/// hi.myrmidon.research.chief-architect.task.{id}
std::string research_dispatch_subject(const std::string& id);

/// Build the hi/v1 dispatch payload for the research pool. Carries the idea
/// and context inline (no GitHub issue exists yet) plus intake/task ids.
nlohmann::json research_dispatch_payload(const nlohmann::json& body, const std::string& id,
                                         const std::string& trace_id);

/// Handle an externally-published research status message (hi.research.{id}).
/// Closes the store item when the payload carries status == "completed".
/// Returns true when a store item was completed.
bool handle_research_status(Store& store, const std::string& subject, const std::string& payload);

}  // namespace nestor
