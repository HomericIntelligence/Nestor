# Changelog

All notable changes to Nestor are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The release process is documented in `docs/RELEASING.md`.

## [Unreleased]

### Added

- `AGENTS.md` documenting multi-agent coordination protocols.
- `docs/data-retention.md` describing the in-memory retention policy.
- `docs/privacy.md` describing privacy and GDPR considerations.
- `docs/ROADMAP.md` listing release milestones.
- `docs/RELEASING.md` documenting the manual release process.
- `docs/audit-logging.md` describing audit-logging conventions.
- `.github/ISSUE_TEMPLATE/` issue templates and `.github/PULL_REQUEST_TEMPLATE.md`.
- `docs` CMake preset and `just docs` recipe wiring Doxygen.
- Expanded `README.md` with prerequisites, env vars, API table, Docker usage.

## [0.1.0] - initial scaffold

- Initial HTTP + NATS scaffold: `POST /v1/research`, `POST /v1/research/:id/complete`,
  `GET /v1/research/stats`, `GET /v1/health`.
- Conan-managed dependencies, CMake presets, gtest suite.

[Unreleased]: https://github.com/HomericIntelligence/Nestor/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/HomericIntelligence/Nestor/releases/tag/v0.1.0
