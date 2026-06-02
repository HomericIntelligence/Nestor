// ProjectNestor route handlers — C++20 with real Store and NATS integration.

#include "projectnestor/routes.hpp"

#include "projectnestor/trace_context.hpp"

#include <optional>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectnestor {

using json = nlohmann::json;

namespace {
constexpr std::size_t kMaxIdeaLen = 4096;
constexpr std::size_t kMaxContextLen = 16384;
constexpr std::size_t kMaxTopLevelFields = 16;

std::optional<std::string> validate_research_body(const json& b) {
  if (!b.is_object()) {
    return "Request body must be a JSON object";
  }
  if (b.size() > kMaxTopLevelFields) {
    return "Request body has too many fields";
  }
  if (!b.contains("idea")) {
    return "Missing required field: idea";
  }
  if (!b["idea"].is_string()) {
    return "Field 'idea' must be a string";
  }
  const auto idea = b["idea"].get<std::string>();
  if (idea.length() > kMaxIdeaLen) {
    return "Field 'idea' exceeds maximum length";
  }
  if (b.contains("context")) {
    if (!b["context"].is_string()) {
      return "Field 'context' must be a string";
    }
    const auto context = b["context"].get<std::string>();
    if (context.length() > kMaxContextLen) {
      return "Field 'context' exceeds maximum length";
    }
  }
  return std::nullopt;
}
}  // namespace

void register_routes(httplib::Server& server, Store& store, NatsClient& nats,
                     RateLimiter& limiter) {
  Store* sp = &store;      // NOLINT
  NatsClient* np = &nats;  // NOLINT
  // lp is captured by value (pointer) so that lambdas do not hold a dangling
  // reference if the lambda outlives this stack frame. limiter is constructed
  // in main() and outlives server.listen() — the invariant is documented in
  // RateLimiter's class-level comment. See cpp-httplib-lambda-capture-ub skill.
  RateLimiter* lp = &limiter;

  // Helper: extract topic field, falling back from "idea" to "topic".
  auto extract_topic = [](const json& j) -> std::string {  // NOLINT
    return j.value("idea", j.value("topic", ""));
  };

  // Throttle helper — call at the top of each handler before touching Store or
  // parsing the body. Returns true if the request is allowed; writes 429 +
  // Retry-After and returns false if rate-limited. (issue #44: DoS prevention)
  auto throttle = [lp](const httplib::Request& req, httplib::Response& res, RouteClass rc) -> bool {
    // Fail-closed: empty remote_addr routes to __unknown__ bucket (never bypass).
    const std::string key = req.remote_addr.empty() ? std::string{"__unknown__"} : req.remote_addr;
    const RateLimitDecision d = lp->check(key, rc);
    if (d.allowed) {
      return true;
    }
    res.status = 429;
    res.set_header("Retry-After", std::to_string(d.retry_after_sec));
    res.set_content(json{{"detail", "rate_limited"}}.dump(), "application/json");
    return false;
  };

  // ── Health ────────────────────────────────────────────────────────────────

  server.Get("/v1/health", [throttle](const httplib::Request& req, httplib::Response& res) {
    if (!throttle(req, res, RouteClass::Default)) {
      return;
    }
    const auto ctx = extract_or_generate(req);
    res.set_header("X-Request-ID", ctx.trace_id);
    res.set_header("traceparent", to_traceparent_header(ctx));
    res.set_content(json{{"status", "ok"}}.dump(), "application/json");
  });

  // ── Research ─────────────────────────────────────────────────────────────

  server.Get("/v1/research/stats",
             [sp, throttle](const httplib::Request& req, httplib::Response& res) {
               if (!throttle(req, res, RouteClass::Default)) {
                 return;
               }
               const auto ctx = extract_or_generate(req);
               res.set_header("X-Request-ID", ctx.trace_id);
               res.set_header("traceparent", to_traceparent_header(ctx));
               res.set_content(sp->get_stats().dump(), "application/json");
             });

  server.Get("/v1/research/:id",
             [sp, throttle](const httplib::Request& req, httplib::Response& res) {
               if (!throttle(req, res, RouteClass::Default)) {
                 return;
               }
               const std::string id = req.path_params.at("id");
               const json item = sp->get_research(id);
               if (item.contains("error")) {
                 res.status = 404;
               }
               res.set_content(item.dump(), "application/json");
             });

  server.Get("/v1/research", [sp, throttle](const httplib::Request& req, httplib::Response& res) {
    if (!throttle(req, res, RouteClass::Default)) {
      return;
    }
    res.set_content(sp->list_research().dump(), "application/json");
  });

  server.Post("/v1/research", [sp, np, extract_topic, throttle](const httplib::Request& req,
                                                                httplib::Response& res) {
    // Gate before parsing body or touching Store (DoS prevention — issue #44).
    if (!throttle(req, res, RouteClass::Research)) {
      return;
    }

    // Reject anything that doesn't declare a JSON content-type.
    // Per RFC 9110 §8.3 the media-type may carry parameters
    // (e.g. "; charset=utf-8"), so match by substring not equality.
    const std::string ct = req.get_header_value("Content-Type");
    if (ct.find("application/json") == std::string::npos) {
      res.status = 415;
      res.set_content(json{{"detail", "Content-Type must be application/json"}}.dump(),
                      "application/json");
      return;
    }
    const auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      res.status = 400;
      res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
      return;
    }

    const auto validation_error = validate_research_body(body);
    if (validation_error) {
      res.status = 400;
      res.set_content(json{{"detail", validation_error.value()}}.dump(), "application/json");
      return;
    }

    const auto ctx = extract_or_generate(req);
    res.set_header("X-Request-ID", ctx.trace_id);
    res.set_header("traceparent", to_traceparent_header(ctx));

    const json result = sp->submit_research(body, ctx.trace_id);
    // Guard: check for capacity error BEFORE any result["id"] access.
    // submit_research returns {"error":"capacity"} when the store is full.
    if (result.contains("error")) {
      res.status = 503;
      res.set_content(json{{"detail", "research capacity exhausted"}}.dump(), "application/json");
      np->publish_log("hi.logs.nestor.research_rejected", "warn",
                      "Research rejected: capacity exhausted", json{{"reason", result["error"]}},
                      ctx.trace_id);
      return;
    }
    const std::string id = result["id"].get<std::string>();
    const std::string topic = extract_topic(body);

    // Publish to hi.research.<id> — graceful degradation if NATS unavailable.
    const std::string subject = "hi.research." + id;
    json payload = body;
    payload["id"] = id;
    payload["status"] = "pending";
    payload["trace_id"] = ctx.trace_id;
    np->publish(subject, payload.dump());

    // Structured log: hi.logs.nestor.research_submitted (ADR-005).
    np->publish_log("hi.logs.nestor.research_submitted", "info",
                    "Research submitted: topic=" + topic,
                    json{{"research_id", id}, {"topic", topic}}, ctx.trace_id);

    res.status = 202;
    res.set_content(result.dump(), "application/json");
  });

  // ── Complete Research ─────────────────────────────────────────────────────

  server.Post(
      "/v1/research/:id/complete",
      [sp, np, extract_topic, throttle](const httplib::Request& req, httplib::Response& res) {
        // Gate before touching Store (DoS prevention — issue #44).
        if (!throttle(req, res, RouteClass::Research)) {
          return;
        }

        const auto ctx = extract_or_generate(req);
        res.set_header("X-Request-ID", ctx.trace_id);
        res.set_header("traceparent", to_traceparent_header(ctx));

        const std::string id = req.path_params.at("id");
        const json updated = sp->complete_research(id);

        if (updated.contains("error")) {
          res.status = 404;
          res.set_content(updated.dump(), "application/json");
          return;
        }

        const std::string topic = extract_topic(updated);
        // Per D2: stored trace_id from submit always wins on completion.
        // Response headers reflect the incoming caller's trace for their logs.
        const std::string stored_trace_id = updated.value("trace_id", "");

        // Structured log: hi.logs.nestor.research_completed (ADR-005).
        np->publish_log("hi.logs.nestor.research_completed", "info",
                        "Research completed: topic=" + topic,
                        json{{"research_id", id}, {"topic", topic}}, stored_trace_id);

        res.set_content(updated.dump(), "application/json");
      });
}

}  // namespace projectnestor
