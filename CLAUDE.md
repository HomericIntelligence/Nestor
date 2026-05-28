# CLAUDE.md — ProjectNestor

## Project Overview

ProjectNestor is the research, ideation, and search service for the HomericIntelligence
distributed agent mesh.

**Role in the pipeline:** User ↔ Odysseus ↔ **Nestor** ↔ Agamemnon ↔ agentic pipeline loop → completion

Nestor receives ideas/tasks from Odysseus and runs them through a research pipeline:

- IDEA → RESEARCH & SEARCH → REVIEW GATE → RESEARCHED BRIEF
- Research myrmidons pull from `hi.research.*` subjects (rate-limited, MaxAckPending=1)
- Uses Telemachy internally for workflow orchestration
- Bidirectional: can escalate to Odysseus/user for review at the review gate
- Hands off approved researched briefs to Agamemnon for planning

## Key Responsibilities

1. **Research phase:** Codebase exploration, doc search, feasibility, prior art
2. **Review gate:** approve → researched brief; clarify → re-enqueue; escalate → Odysseus
3. **Handoff:** Researched briefs passed to Agamemnon for planning breakdown

## Architecture

All communication flows **through ProjectKeystone** (invisible transport):

- Local (intra-host): BlazingMQ + C++20 MessageBus
- Cross-host: NATS JetStream via nats.c v3.12.0 over Tailscale

Relevant NATS subjects:

- `hi.research.>` — research task queue (PULL consumers, research myrmidons pull from here)
- `hi.pipeline.>` — pipeline state updates (pub to Odysseus)

## Development Guidelines

- Language: C++20 exclusively
- Build: `cmake --preset debug` / `cmake --build --preset debug`
- Test: `ctest --preset debug`
- All tool invocations via `scripts/` wrappers
- Never `--no-verify`. Never merge with red CI.
- PRs to `main` require ≥1 approving review; never self-merge. See `docs/governance/branch-protection.md`.
