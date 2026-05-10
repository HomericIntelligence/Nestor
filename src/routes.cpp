// ProjectNestor route handlers — C++20 with real Store and NATS integration.

#include "projectnestor/routes.hpp"

#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectnestor {

using json = nlohmann::json;

void register_routes(httplib::Server& server, Store& store, NatsClient& nats) {
  Store* sp = &store;
  NatsClient* np = &nats;

  // Helper: extract topic field, falling back from "idea" to "topic".
  auto extract_topic = [](const json& j) -> std::string {
    return j.value("idea", j.value("topic", ""));
  };

  // ── Health ────────────────────────────────────────────────────────────────

  server.Get("/v1/health", [](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_content(json{{"status", "ok"}}.dump(), "application/json");
  });

  // ── Research ─────────────────────────────────────────────────────────────

  server.Get("/v1/research/stats", [sp](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_content(sp->get_stats().dump(), "application/json");
  });

  server.Post("/v1/research", [sp, np, extract_topic](const httplib::Request& req, httplib::Response& res) {
    const auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      res.status = 400;
      res.set_content(json{{"detail", "Invalid JSON"}}.dump(), "application/json");
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
    np->publish(subject, payload.dump());

    // Structured log: hi.logs.nestor.research_submitted (ADR-005).
    np->publish_log("hi.logs.nestor.research_submitted", "info",
                    "Research submitted: topic=" + topic,
                    json{{"research_id", id}, {"topic", topic}});

    res.status = 202;
    res.set_content(result.dump(), "application/json");
  });

  // ── Complete Research ─────────────────────────────────────────────────────

  server.Post("/v1/research/:id/complete",
              [sp, np, extract_topic](const httplib::Request& req, httplib::Response& res) {
                const std::string id = req.path_params.at("id");
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
                                json{{"research_id", id}, {"topic", topic}});

                res.set_content(updated.dump(), "application/json");
              });
}

}  // namespace projectnestor
