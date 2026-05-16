# Audit Logging

This document describes ProjectNestor's audit-logging conventions for
security-relevant events.

## Goals

- Make every security-relevant request observable through the
  `hi.logs.nestor.*` NATS subject family (ADR-005).
- Avoid leaking user-submitted payload contents into log subjects, message
  bodies, or operator-facing dashboards beyond what `docs/privacy.md`
  permits.

## Event taxonomy

| Subject | Severity | Emitted when |
|---|---|---|
| `hi.logs.nestor.research_submitted` | info | `POST /v1/research` returns 202 |
| `hi.logs.nestor.research_completed` | info | `POST /v1/research/:id/complete` returns 200 |
| `hi.logs.nestor.research_invalid_input` | warn | `POST /v1/research` returns 400 or 415 |
| `hi.logs.nestor.research_not_found` | warn | `POST /v1/research/:id/complete` returns 404 |
| `hi.logs.nestor.unknown_route` | warn | request to a path not registered in `routes.cpp` |

The first two events already emit (see `src/routes.cpp`). The remaining three
are tracked in the issue backlog and will be added with corresponding tests;
this document defines their contract so that implementations stay
consistent.

## Payload shape

Each event carries a JSON object with at least:

- `level` — one of `info`, `warn`, `error`.
- `message` — short human-readable summary.
- `research_id` — present for events with a known item.
- `topic` — present where the request supplied one; truncated to 64 chars.
- `client_ip` — when reverse-proxy headers expose it; never required.

User-submitted free-form text (`idea`, `context`) **must not** be embedded.

## Retention

Audit log retention is governed by the NATS event-bridge subscriber
(ProjectArgus). ProjectNestor itself does not persist audit logs locally.

## Review

This policy is reviewed whenever a new security-sensitive endpoint is
added to ProjectNestor.
