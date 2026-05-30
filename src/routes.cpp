// ProjectNestor route handlers — C++20 with real Store and NATS integration.
// Issues addressed:
//   #40/#65 — auth check per-route (Bearer token, /v1/health exempt)
//   #41     — body-size cap before json::parse (prevents memory exhaustion)
//   #44     — rate limiting via token bucket (pre-routing handler)
//   #49     — X-Correlation-ID threading into responses and NATS log payloads
//   #64     — GET /v1/research (paginated) and GET /v1/research/:id
//   #67     — completion body validation (requires "summary" field)

#include "projectnestor/routes.hpp"

#include "projectnestor/auth_middleware.hpp"
#include "projectnestor/correlation_id.hpp"
#include "projectnestor/rate_limiter.hpp"
#include "projectnestor/request_limits.hpp"

#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectnestor {

using json = nlohmann::json;

void register_routes(httplib::Server& server, Store& store, NatsClient& nats, AuthMiddleware& auth,
                     RateLimiter& rate) {
  Store* sp = &store;
  NatsClient* np = &nats;

  // Helper: extract topic field, falling back from "idea" to "topic".
  auto extract_topic = [](const json& j) -> std::string {
    return j.value("idea", j.value("topic", ""));
  };

  // ── Pre-routing: rate limiting ─────────────────────────────────────────────
  // httplib supports one pre_routing_handler; we apply rate limiting here.
  // Auth is applied per-route via auth.check_request() so that /v1/health
  // remains unauthenticated while all other endpoints are protected.
  server.set_pre_routing_handler(
      [&rate](const httplib::Request& req,
              httplib::Response& res) -> httplib::Server::HandlerResponse {
        // /v1/health is always exempt from rate limiting.
        if (req.path == "/v1/health") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (!rate.allow(req)) {
          res.status = 429;
          res.set_content(R"({"detail":"Too Many Requests"})", "application/json");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  // ── Health ────────────────────────────────────────────────────────────────

  server.Get("/v1/health", [](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_content(json{{"status", "ok"}}.dump(), "application/json");
  });

  // ── Research list (paginated) ─────────────────────────────────────────────
  // Issue #64: GET /v1/research?offset=0&limit=20

  server.Get("/v1/research", [sp, &auth](const httplib::Request& req, httplib::Response& res) {
    const std::string cid = get_or_generate_correlation_id(req);
    set_correlation_id_header(res, cid);

    if (!auth.check_request(req, res)) {
      return;
    }

    std::size_t offset = 0;
    std::size_t limit = 20;
    if (req.has_param("offset")) {
      try {
        const int v = std::stoi(req.get_param_value("offset"));
        if (v >= 0) {
          offset = static_cast<std::size_t>(v);
        }
      } catch (...) {
      }
    }
    if (req.has_param("limit")) {
      try {
        const int v = std::stoi(req.get_param_value("limit"));
        if (v > 0 && v <= 100) {
          limit = static_cast<std::size_t>(v);
        }
      } catch (...) {
      }
    }
    res.set_content(sp->list(offset, limit).dump(), "application/json");
  });

  // ── Research stats ────────────────────────────────────────────────────────
  // IMPORTANT: register /v1/research/stats BEFORE /v1/research/:id so the
  // literal path takes priority over the parameterised pattern in cpp-httplib.

  server.Get("/v1/research/stats",
             [sp, &auth](const httplib::Request& req, httplib::Response& res) {
               const std::string cid = get_or_generate_correlation_id(req);
               set_correlation_id_header(res, cid);

               if (!auth.check_request(req, res)) {
                 return;
               }

               res.set_content(sp->get_stats().dump(), "application/json");
             });

  // ── Research by ID ────────────────────────────────────────────────────────
  // Issue #64: GET /v1/research/:id
  // Registered AFTER /v1/research/stats so "stats" is not matched as an :id.

  server.Get("/v1/research/:id", [sp, &auth](const httplib::Request& req, httplib::Response& res) {
    const std::string cid = get_or_generate_correlation_id(req);
    set_correlation_id_header(res, cid);

    if (!auth.check_request(req, res)) {
      return;
    }

    const std::string id = req.path_params.at("id");
    const json item = sp->get(id);
    if (item.contains("error")) {
      res.status = 404;
    }
    res.set_content(item.dump(), "application/json");
  });

  // ── Submit Research ───────────────────────────────────────────────────────

  server.Post("/v1/research", [sp, np, &auth, extract_topic](const httplib::Request& req,
                                                             httplib::Response& res) {
    const std::string cid = get_or_generate_correlation_id(req);
    set_correlation_id_header(res, cid);

    if (!auth.check_request(req, res)) {
      return;
    }

    // Issue #41: Reject oversized bodies before json::parse.
    if (req.body.size() > kMaxBodyBytes) {
      res.status = 413;
      res.set_content(json{{"detail", "Request body exceeds maximum allowed size"}}.dump(),
                      "application/json");
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

    // Issue #41: Validate required fields and types.
    if (const auto err = validate_research_submission(body)) {
      res.status = err->status;
      res.set_content(json{{"detail", err->detail}}.dump(), "application/json");
      return;
    }

    const json result = sp->submit_research(body);
    const std::string id = result["id"].get<std::string>();
    const std::string topic = extract_topic(body);

    // Publish to hi.research.<id> — graceful degradation if NATS unavailable.
    const std::string subject = "hi.research." + id;
    json payload = body;
    payload["id"] = id;
    payload["status"] = "pending";
    payload["correlation_id"] = cid;
    np->publish(subject, payload.dump());

    // Structured log: hi.logs.nestor.research_submitted (ADR-005).
    // Issue #49: include correlation_id in log metadata.
    np->publish_log("hi.logs.nestor.research_submitted", "info",
                    "Research submitted: topic=" + topic,
                    json{{"research_id", id}, {"topic", topic}, {"correlation_id", cid}});

    res.status = 202;
    res.set_content(result.dump(), "application/json");
  });

  // ── Complete Research ─────────────────────────────────────────────────────

  server.Post(
      "/v1/research/:id/complete",
      [sp, np, &auth, extract_topic](const httplib::Request& req, httplib::Response& res) {
        const std::string cid = get_or_generate_correlation_id(req);
        set_correlation_id_header(res, cid);

        if (!auth.check_request(req, res)) {
          return;
        }

        const std::string id = req.path_params.at("id");

        // Issue #67: Require a non-empty body with a "summary" field.
        if (req.body.size() > kMaxBodyBytes) {
          res.status = 413;
          res.set_content(json{{"detail", "Request body exceeds maximum allowed size"}}.dump(),
                          "application/json");
          return;
        }

        // Parse body for completion metadata.
        json completion_body = json::object();
        if (!req.body.empty()) {
          const std::string ct = req.get_header_value("Content-Type");
          if (ct.find("application/json") == std::string::npos) {
            res.status = 415;
            res.set_content(json{{"detail", "Content-Type must be application/json"}}.dump(),
                            "application/json");
            return;
          }
          completion_body = json::parse(req.body, nullptr, false);
          if (completion_body.is_discarded()) {
            res.status = 400;
            res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
            return;
          }
        }

        // Issue #67: Validate completion metadata.
        if (const auto err = validate_completion(completion_body)) {
          res.status = err->status;
          res.set_content(json{{"detail", err->detail}}.dump(), "application/json");
          return;
        }

        const json updated = sp->complete_research(id);

        if (updated.contains("error")) {
          res.status = 404;
          res.set_content(updated.dump(), "application/json");
          return;
        }

        const std::string topic = extract_topic(updated);

        // Structured log: hi.logs.nestor.research_completed (ADR-005).
        np->publish_log("hi.logs.nestor.research_completed", "info",
                        "Research completed: topic=" + topic,
                        json{{"research_id", id}, {"topic", topic}, {"correlation_id", cid}});

        res.set_content(updated.dump(), "application/json");
      });
}

}  // namespace projectnestor
