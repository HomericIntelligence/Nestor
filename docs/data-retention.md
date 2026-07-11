# Data Retention and Deletion Policy

This document describes the data retention and deletion policy for
Nestor.

## Scope

Nestor stores research items submitted via `POST /v1/research`. Each
item contains an optional user-provided `idea` string, an optional `context`
string, an opaque server-generated `id`, and a lifecycle `status`.

## Retention

Research items are retained in-memory with two bounds:

- **Hard cap:** At most `NESTOR_MAX_ITEMS` live entries (default: 10,000).
  When the store is full, `POST /v1/research` returns HTTP 503 rather than
  silently evicting existing data.
- **Pending TTL:** Pending entries older than `NESTOR_PENDING_TTL_SECONDS`
  (default: 86400 — 24 hours) are swept on the *next* `POST /v1/research`
  call (lazy eviction, not autonomous). During idle periods, expired-but-not-yet-
  swept entries persist in the store until the next submit request arrives, but
  they still count against `max_items`.
- **Eager erase on completion:** Completed entries are removed from the store
  immediately when `complete_research` transitions the item, not at process
  restart.

The eviction counter is exposed via `GET /v1/research/stats` as the `expired`
field, enabling operators to observe how many pending items have been silently
dropped due to TTL.

There is currently **no on-disk persistence**; restarting the Nestor
process clears all stored research items regardless of the above bounds.

## Deletion

There is currently no `DELETE /v1/research/:id` endpoint. However, two
self-service deletion paths exist:

- **Completed items** self-erase immediately on the `complete_research`
  transition — no operator action required.
- **Pending items** self-erase on the next submit request after their TTL
  expires — operator restart is no longer the only deletion path.

For immediate deletion of a pending item, users must request operator
intervention. A user-facing deletion endpoint is tracked in the issue backlog
and will be implemented before Nestor stores user data on persistent
media.

## Data subject rights

Until a deletion endpoint exists, Nestor accepts user text on the
explicit understanding (documented in `docs/privacy.md`) that operators may
clear the in-memory store at any time. Users who require GDPR-style
right-to-erasure guarantees must avoid submitting personal data to this
service.

## Review

This policy is reviewed whenever a persistent storage backend is added to
Nestor or whenever a new GDPR-relevant feature lands.
