# Privacy and Data Handling

Nestor accepts free-form user text via `POST /v1/research`
(`idea` and optional `context` fields). This document describes how
that data is handled.

The separate, explicitly configured Fleet intake endpoint has the additional
GitHub data flow described below. Legacy in-memory deletion statements do not
apply to Fleet records or work issues.

## What is collected

- The verbatim `idea` and `context` strings.
- A server-generated opaque `id`.
- Submission and completion timestamps (implicit, via NATS event timestamps).
- A best-effort topic field used for structured logging.

No client IP, user-agent, or identity material is logged by Nestor
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

### Fleet intake

`POST /v1/research/intakes` sends the supplied publishable title/body to the
configured work repository's GitHub issues. The issue receives a stable intake
marker. The configured private state repository stores only the intake ID,
canonical repository, request/body digests, phase, timestamps, creation attempt
ID, and confirmed issue reference/receipt. These records use Git history.
No raw private interview, conversation transcript, or backend credential belongs
in either metadata or the publishable request. Digests are identity metadata,
not anonymization. See [the API contract](fleet-intake.md) and
[retention policy](data-retention.md#fleet-intake-retention).

## GDPR considerations

Nestor is provided primarily for internal HomericIntelligence
research workflows. Users **should not** submit personal data (their own or
anyone else's) via this API.

### Lawful basis

When Nestor is operated on personal data the lawful basis is
expected to be **legitimate interest** for internal research/operational
use, unless the operator has obtained explicit consent.

### Data subject rights

The legacy in-memory store can be cleared by restarting Nestor. This does not
erase data already delivered to subscribers. Fleet intake records and issues
survive restart and need operator-managed GitHub retention and erasure handling;
the Fleet API has no deletion endpoint. See `docs/data-retention.md`.

### International transfers

Legacy NATS subscribers may transfer data; consult their operators. Fleet intake
sends issue content and metadata to GitHub over HTTPS. Operators must assess the
hosting, access, and data handling arrangements for their configured repositories.

## Contact

Data privacy inquiries should be raised as a GitHub issue against this
repository with the `privacy` label, or via the channels described in
`SECURITY.md`.
