# Data Retention and Deletion Policy

This document describes the data retention and deletion policy for
ProjectNestor.

## Scope

ProjectNestor stores research items submitted via `POST /v1/research`. Each
item contains an optional user-provided `idea` string, an optional `context`
string, an opaque server-generated `id`, and a lifecycle `status`.

## Retention

Research items are retained in the in-memory `Store` for the lifetime of the
process. There is currently **no on-disk persistence**; restarting the
ProjectNestor process clears all stored research items.

Operational target retention is **24 hours** of running time; long-running
deployments must restart at least daily to bound memory growth. A bounded
in-memory store with explicit TTL eviction is tracked in the issue backlog
(see `[MAJOR] §9: Unbounded in-memory research_items_ map`).

## Deletion

There is currently no `DELETE /v1/research/:id` endpoint. Users wishing to
delete a submitted research item must request operator intervention; the
operator can clear all items by restarting the ProjectNestor process.

A user-facing deletion endpoint is tracked in the issue backlog and will be
implemented before ProjectNestor stores user data on persistent media.

## Data subject rights

Until a deletion endpoint exists, ProjectNestor accepts user text on the
explicit understanding (documented in `docs/privacy.md`) that operators may
clear the in-memory store at any time. Users who require GDPR-style
right-to-erasure guarantees must avoid submitting personal data to this
service.

## Review

This policy is reviewed whenever a persistent storage backend is added to
ProjectNestor or whenever a new GDPR-relevant feature lands.
