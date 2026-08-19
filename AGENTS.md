# AGENTS.md — Nestor

This document is the sole authoritative agent contract for Nestor. It
specifies the multi-agent coordination protocols for Nestor within the
HomericIntelligence distributed agent mesh, plus single-agent operational
conventions (project overview, architecture, and development guidelines).

## Project overview

Nestor is the research, ideation, and search service for the
HomericIntelligence distributed agent mesh. It receives ideas/tasks from
Odysseus and runs them through a research pipeline:

- IDEA → RESEARCH & SEARCH → REVIEW GATE → RESEARCHED BRIEF
- Research myrmidons pull from `hi.research.*` subjects (rate-limited,
  MaxAckPending=1)
- Uses Telemachy internally for workflow orchestration
- Bidirectional: can escalate to Odysseus/user for review at the review gate
- Hands off approved researched briefs to Agamemnon for planning

### Key responsibilities

1. **Research phase:** Codebase exploration, doc search, feasibility, prior art
2. **Review gate:** approve → researched brief; clarify → re-enqueue;
   escalate → Odysseus
3. **Handoff:** Researched briefs passed to Agamemnon for planning breakdown

## Role boundary

Nestor is the research and ideation upstream service. It transforms raw
user ideas into researched briefs that ProjectAgamemnon plans and executes.

```
User → Odysseus → Nestor → Agamemnon → agentic pipeline loop → completion
```

Nestor is the only agent in the mesh authorised to:

- Accept unstructured user ideas via `POST /v1/research`.
- Publish `hi.research.<id>` events for downstream planners.
- Emit `hi.logs.nestor.*` structured logs (ADR-005 subject schema).

Nestor must **not**:

- Plan, dispatch, or execute tasks (that is Agamemnon's role).
- Provision agents (that is ProjectTelemachy's role).
- Make architecture decisions (that is ProjectOdyssey / ADRs).

## Architecture

All communication flows **through ProjectKeystone** (invisible transport):

- Local (intra-host): BlazingMQ + C++20 MessageBus
- Cross-host: NATS JetStream via nats.c v3.12.0 over Tailscale

Relevant NATS subjects:

- `hi.research.>` — research task queue (PULL consumers, research myrmidons
  pull from here)
- `hi.pipeline.>` — pipeline state updates (pub to Odysseus)

## Handoff contract

Nestor publishes research state transitions to the `hi.research.<id>` subject
that ProjectAgamemnon subscribes to (per Agent role boundaries, §44).

**Pending event** (on submit):

```json
{
  "id": "<uuid>",
  "idea": "<verbatim user text>",
  "context": "<optional context>",
  "status": "pending",
  "submitted_at": "<iso8601>"
}
```

**Completed event** (on completion, extends the same subject):

```json
{
  "id": "<uuid>",
  "idea": "<original idea>",
  "context": "<original context>",
  "status": "completed",
  "submitted_at": "<iso8601>",
  "completed_at": "<iso8601>",
  "summary": "<optional completion summary>",
  "results": { "optional": "completion metadata as object" },
  "references": ["optional", "array", "of", "references"]
}
```

The structured log `hi.logs.nestor.research_completed` is also emitted (ADR-005)
for audit trail purposes, carrying `{research_id, topic, has_summary, result_count, reference_count}`.

## Agent role boundaries

| Agent | Owns | Reads from Nestor |
| --- | --- | --- |
| ProjectAgamemnon | planning, dispatch | `hi.research.*` |
| ProjectArgus | observability | `hi.logs.nestor.*` |
| ProjectHermes | NATS event bridge | all `hi.*` |

## Inter-agent message contracts

All Nestor-published messages conform to ADR-005 NATS subject schema. Field
names are stable across minor versions; breaking changes require an ADR
reference and a major-version bump.

## Development guidelines

- Language: C++20 exclusively
- Build: `cmake --preset debug` / `cmake --build --preset debug`
- Test: `ctest --preset debug`
- All tool invocations via `scripts/` wrappers
- Never `--no-verify`. Never merge with red CI.
- PRs to `main` are gated by required status checks, resolved review
  conversations, and linear history; live protection requires zero approving
  reviews and does not dismiss stale reviews. Never self-merge. Independent
  human review of workflow changes is an external gate for the staged
  merge-queue rollout, not a live protection rule. See
  `docs/governance/branch-protection.md` and
  `docs/governance/merge-queue.md`.

## Design Philosophy

The architecture above follows a small set of design principles inherited from
**ProjectOdyssey**, applied to a native C++ agent:

- **Hard guarantees over convenience (KISS).** The server core is C++ with
  `-Werror`, deterministic Conan/pixi builds, and explicit failure modes — a
  compile-time guarantee beats a runtime check.
- **Fail closed (POLA).** TLS material, path handling, and untrusted input
  default to rejection; validation happens at the boundary, once
  (`src/tls_config.cpp`), not per call site.
- **Minimal surface (YAGNI).** Dependencies are added only when a protocol
  requirement forces them; the NATS and TLS integrations are the full scope.
- **One responsibility per component (DRY / boundaries).** Research logic,
  transport, and configuration are separable modules with single-owner
  responsibility.


## See also

- `docs/adr/` — architectural decisions (when published).
- Odysseus `docs/adr/005-nats-subject-schema.md`.
