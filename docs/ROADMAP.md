# Nestor Roadmap

This is the working roadmap for Nestor. It complements the GitHub
issue tracker (the authoritative work queue) by grouping work into
milestones and noting prioritisation.

## How this document is used

- Open issues map to one of the milestones below.
- Closed milestones are kept for historical context; once a milestone closes
  it is appended to *Past releases*.
- Definition of done for any milestone: all linked issues closed, CI green
  on `main`, `CHANGELOG.md` updated, tag pushed.

## Current — v0.1.x (stabilisation)

Goal: make the existing API production-shapeable; no new endpoints.

Tracked priorities:

- Input validation on `POST /v1/research` (#41).
- Authentication on the public API (#40, #65).
- Bounded `research_items_` map with eviction (#48, #21).
- NATS reconnection logic (#47).
- Sanitiser / warning options actually take effect (#22, #23, #43).

## Next — v0.2.0 (operability)

Goal: make Nestor pleasant to run in a Nomad fleet.

Tracked priorities:

- TLS terminator inside the binary (#42).
- Distributed tracing / correlation IDs (#49).
- Rate limiting (#44).
- Audit logging for security-relevant events (#46).
- Doxygen documentation publishable to CI artifact (#17).

## Later — v0.3.0 (lifecycle)

Goal: real release process, real data lifecycle.

Tracked priorities:

- Release workflow that bumps the 4 hardcoded versions (#55).
- `DELETE /v1/research/:id` endpoint + persistent retention store (#69).
- GET endpoints for research items (#64).

## Past releases

(none — pre-v0.1.0)

## Milestones

GitHub milestones mirror the headings above:

- `v0.1.x — stabilisation`
- `v0.2.0 — operability`
- `v0.3.0 — lifecycle`

The Definition of Done in `CONTRIBUTING.md` (when added) governs what
"complete" means inside each milestone.
