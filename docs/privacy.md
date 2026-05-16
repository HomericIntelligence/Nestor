# Privacy and Data Handling

ProjectNestor accepts free-form user text via `POST /v1/research`
(`idea` and optional `context` fields). This document describes how
that data is handled.

## What is collected

- The verbatim `idea` and `context` strings.
- A server-generated opaque `id`.
- Submission and completion timestamps (implicit, via NATS event timestamps).
- A best-effort topic field used for structured logging.

No client IP, user-agent, or identity material is logged by ProjectNestor
itself. Upstream gateways may log such data separately.

## Where it goes

1. **In-memory store.** Research items live in process memory until the
   process is restarted. See `docs/data-retention.md`.
2. **NATS event bus.** A `hi.research.<id>` event carrying the submitted
   payload is published for downstream agents (ProjectAgamemnon and other
   subscribers). Subscribers may persist or forward this data independently;
   downstream retention is governed by each subscriber's policy.
3. **Structured logs.** Topic and `research_id` are published as
   `hi.logs.nestor.*` events. The full `idea` text is **not** logged.

## GDPR considerations

ProjectNestor is provided primarily for internal HomericIntelligence
research workflows. Users **should not** submit personal data (their own or
anyone else's) via this API.

### Lawful basis

When ProjectNestor is operated on personal data the lawful basis is
expected to be **legitimate interest** for internal research/operational
use, unless the operator has obtained explicit consent.

### Data subject rights

Until a persistent storage backend and a DELETE endpoint exist, the
practical mechanism for honouring an erasure request is for an operator
to restart the ProjectNestor process. See `docs/data-retention.md`.

### International transfers

ProjectNestor does not, on its own, transfer data internationally. NATS
subscribers may; consult the operator of each subscriber.

## Contact

Data privacy inquiries should be raised as a GitHub issue against this
repository with the `privacy` label, or via the channels described in
`SECURITY.md`.
