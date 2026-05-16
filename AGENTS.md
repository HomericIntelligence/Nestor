# AGENTS.md — ProjectNestor

This document specifies the multi-agent coordination protocols for ProjectNestor
within the HomericIntelligence distributed agent mesh.

## Role boundary

ProjectNestor is the research and ideation upstream service. It transforms raw
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

## Handoff contract

When research is submitted Nestor publishes to NATS subject `hi.research.<id>`
with the body:

```json
{
  "id": "<uuid>",
  "idea": "<verbatim user text>",
  "context": "<optional context>",
  "status": "pending"
}
```

When research is completed Nestor emits
`hi.logs.nestor.research_completed` carrying `{research_id, topic}`.

## Agent role boundaries

| Agent | Owns | Reads from Nestor |
|---|---|---|
| ProjectAgamemnon | planning, dispatch | `hi.research.*` |
| ProjectArgus | observability | `hi.logs.nestor.*` |
| ProjectHermes | NATS event bridge | all `hi.*` |

## Inter-agent message contracts

All Nestor-published messages conform to ADR-005 NATS subject schema. Field
names are stable across minor versions; breaking changes require an ADR
reference and a major-version bump.

## See also

- `CLAUDE.md` — single-agent operational conventions.
- `docs/adr/` — architectural decisions (when published).
- Odysseus `docs/adr/005-nats-subject-schema.md`.
