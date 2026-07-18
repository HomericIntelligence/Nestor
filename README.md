# Nestor

[![Build & Test](https://github.com/HomericIntelligence/Nestor/actions/workflows/build-test.yml/badge.svg)](https://github.com/HomericIntelligence/Nestor/actions/workflows/build-test.yml)
[![Code Coverage](https://github.com/HomericIntelligence/Nestor/actions/workflows/code-coverage.yml/badge.svg)](https://github.com/HomericIntelligence/Nestor/actions/workflows/code-coverage.yml)
[![Static Analysis](https://github.com/HomericIntelligence/Nestor/actions/workflows/static-analysis.yml/badge.svg)](https://github.com/HomericIntelligence/Nestor/actions/workflows/static-analysis.yml)

Research, ideation, and search for the HomericIntelligence distributed agent mesh.

Part of [Odysseus](https://github.com/HomericIntelligence/Odysseus) — the HomericIntelligence meta-repo.

## Role

```
User ↔ Odysseus ↔ Nestor ↔ Agamemnon ↔ agentic pipeline loop → completion
```

Nestor transforms raw ideas into researched briefs that Agamemnon can plan and execute.

## Prerequisites

- C++20 toolchain (GCC 12+ or Clang 15+; CI tests against GCC 14 / Clang 17)
- CMake 3.20+
- [Conan](https://docs.conan.io/) 2.x — provides `cpp-httplib`, `nlohmann_json`, and `gtest`
- [uv](https://docs.astral.sh/uv/) — manages the pinned build toolchain (CMake, Ninja, Conan, gcovr, pre-commit) as locked PyPI wheels (Odysseus ADR-018)
- [just](https://github.com/casey/just) — recipe runner
- (Optional) [Doxygen](https://www.doxygen.nl/) — for `just docs`
- (Optional) [Podman](https://podman.io/) or Docker — for container builds

## Building

Conan provides `cpp-httplib`, `nlohmann_json`, and `gtest`. Install
dependencies first — the CMake presets expect the Conan toolchain at
`build/debug/`.

First-time setup: `uv sync` installs the locked build toolchain (CMake, Ninja,
Conan, gcovr); `conan profile detect --exist-ok` then generates your host's base
profile (the committed profiles extend it via `include(default)`). `just deps`
runs both automatically.

```bash
uv sync            # install the locked CMake/Ninja/Conan/gcovr toolchain
just deps          # bootstrap + conan install --profile:all=conan/profiles/nestor-debug
uv run cmake --preset debug
uv run cmake --build --preset debug
uv run ctest --preset debug
```

Or via the all-in-one recipes:

```bash
just build
just test
```

**Non-Linux hosts (ARM64, macOS):** uv installs the CMake/Ninja/Conan wheels
cross-platform. If a wheel is unavailable for your platform, install Conan,
CMake, and Ninja directly and follow `conan/profiles/README.md` for the manual
build path.

## Running

```bash
export NESTOR_AUTH_TOKEN="your-secret-token"
./build/debug/bin/nestor      # or run via container, below
```

By default the HTTP server listens on `0.0.0.0:8080` and publishes events to
NATS at `nats://127.0.0.1:4222`.

All `/v1/*` endpoints require bearer-token authentication (see **Configuration** below).

## Configuration

### Authentication (Required)

All endpoints require bearer-token authentication by default. Set these environment variables:

- **`NESTOR_AUTH_TOKEN`** — your API bearer token (required)
- **`NESTOR_AUTH_MODE`** — authentication mode: `"required"` or `"none"` (case-sensitive lowercase; defaults to `"required"`)

The server fails to start if `NESTOR_AUTH_TOKEN` is missing or empty in `required` mode.

Example request:

```bash
curl -H "Authorization: Bearer your-secret-token" http://localhost:8080/v1/health
# {"status":"ok"}
```

### Other Environment variables

| Variable | Default | Description |
| --- | --- | --- |
| `NESTOR_PORT` | `8081` | HTTP server port |
| `NATS_URL` | `nats://127.0.0.1:4222` | NATS broker URL for event publication |
| `NESTOR_AUTH_TOKEN` | *(required)* | Bearer token for authentication |
| `NESTOR_AUTH_MODE` | `required` | Authentication mode: `"required"` or `"none"` |

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/v1/health` | Liveness probe; returns `{"status":"ok"}` |
| `GET` | `/v1/research/stats` | In-memory store counters |
| `POST` | `/v1/research` | Submit `{idea, context?}` JSON; returns `202` with `{id, status:"pending"}` |
| `POST` | `/v1/research/:id/complete` | Mark a research item complete; accepts optional JSON body with `summary` (string), `results` (object), `references` (array of strings); republishes to `hi.research.<id>` for Agamemnon |

See `src/routes.cpp` for the canonical contract; `just docs` builds Doxygen
API documentation under `build/docs/`.

## Docker

A minimal Dockerfile is provided:

```bash
podman build -t nestor .
podman run --rm -p 8080:8080 \
  -e NATS_URL=nats://host.docker.internal:4222 \
  nestor
```

## Documentation

- `CLAUDE.md` — agent operational conventions.
- `AGENTS.md` — multi-agent coordination protocol.
- `CONTRIBUTING.md` — contribution workflow.
- `SECURITY.md` — vulnerability disclosure.
- `CODE_OF_CONDUCT.md` — community guidelines.
- `CHANGELOG.md` — release notes.
- `docs/privacy.md`, `docs/data-retention.md` — data handling policies.
- `docs/ROADMAP.md` — release roadmap.
- `docs/RELEASING.md` — release process.

## License

MIT — see `LICENSE`.
