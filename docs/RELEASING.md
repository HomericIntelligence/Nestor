# Releasing ProjectNestor

This document describes the **manual** release process for ProjectNestor.
Automation is tracked separately (see `docs/ROADMAP.md`, milestone v0.3.0).

## Version sources of truth

The version `MAJOR.MINOR.PATCH` is currently duplicated in four places:

1. `CMakeLists.txt` — `project(... VERSION X.Y.Z ...)`.
2. `conanfile.py` — `version = "X.Y.Z"`.
3. `include/projectnestor/version.hpp` — `constexpr` literals.
4. `pixi.toml` — `version = "X.Y.Z"` under `[project]`.

A future automation will collapse these to one. Until then, **all four must
be bumped in the same commit**.

## Release procedure

1. Decide the bump kind (`MAJOR`, `MINOR`, `PATCH`) following SemVer.
   - Breaking API or NATS subject changes → `MAJOR`.
   - New endpoints or new optional behaviour → `MINOR`.
   - Bug fixes, doc-only, refactors with no behaviour change → `PATCH`.
2. Update the four version sources above in a single commit:
   `chore(release): bump version to X.Y.Z`.
3. Update `CHANGELOG.md`:
   - Move accumulated `Unreleased` entries under `## [X.Y.Z] - YYYY-MM-DD`.
   - Add a fresh empty `## [Unreleased]` block above it.
4. Open a PR titled `chore(release): X.Y.Z` and wait for CI green.
5. After squash-merge, create an annotated git tag on the squash commit:
   `git tag -a vX.Y.Z -m "Release X.Y.Z"` and `git push origin vX.Y.Z`.
6. Create a matching GitHub Release pointing at the tag, copying the
   `CHANGELOG.md` entry as the release notes.
7. Announce the release in the Odysseus integration channel.

## Versioning policy

ProjectNestor follows [Semantic Versioning 2.0.0](https://semver.org/). The
public surface area governed by SemVer is:

- HTTP API paths, request shape, response shape.
- NATS subjects emitted by the service (`hi.research.*`, `hi.logs.nestor.*`).
- The C++ API exposed via `include/projectnestor/`.

Internal implementation details (file layout, private classes, CMake
internals) are **not** part of the public surface.

## Pre-1.0 caveat

While the version remains `0.y.z` the public surface may change in any
`MINOR` bump. Operators should pin to a specific `MINOR` until v1.0.0.
