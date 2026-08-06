set shell := ["bash", "-c"]

# The C++ build toolchain (cmake / ninja / conan / gcovr / pre-commit) is
# managed by uv as locked PyPI wheels (Odysseus ADR-018). Every recipe runs its
# tools through `uv run` so they resolve to the versions pinned in uv.lock.
# The compiler (gcc/g++), clang-tidy/clang-format and libssl-dev come from the
# system (apt); conan's `include(default)` profile autodetects the system gcc.

default:
  @just --list

# Bootstrap: create ~/.conan2/profiles/default if it does not exist (no-op if present)
_conan-bootstrap:
  uv run conan profile detect --exist-ok

# Install Conan dependencies (cpp-httplib, nlohmann_json, gtest) — Debug
deps: _conan-bootstrap
  uv run conan install . --output-folder=build/debug --profile:all=conan/profiles/nestor-debug --build=missing

# Install Conan dependencies for release
deps-release: _conan-bootstrap
  uv run conan install . --output-folder=build/release --profile:all=conan/profiles/nestor-release --build=missing

build: deps
  uv run cmake --preset debug && uv run cmake --build --preset debug

test:
  uv run ctest --preset debug --output-on-failure

lint:
  ./scripts/lint.sh

format:
  ./scripts/format.sh

format-check:
  ./scripts/format.sh --check

coverage: _conan-bootstrap
  uv run conan install . --output-folder=build/coverage --profile:all=conan/profiles/nestor-debug --build=missing && \
  uv run cmake --preset coverage && uv run cmake --build --preset coverage && ./scripts/coverage.sh

clean:
  rm -rf build install

ci:
  uv run cmake --preset ci && uv run cmake --build --preset ci && uv run ctest --preset ci

# Generate Doxygen API documentation under build/docs/
docs: _conan-bootstrap
  uv run conan install . --output-folder=build/docs --profile:all=conan/profiles/nestor-debug --build=missing && \
  uv run cmake --preset docs && uv run cmake --build --preset docs

# Install Conan dependencies for AddressSanitizer
deps-asan:
  uv run conan install . --output-folder=build/asan --profile=conan/profiles/debug --build=missing

# Install Conan dependencies for ThreadSanitizer
deps-tsan:
  uv run conan install . --output-folder=build/tsan --profile=conan/profiles/debug --build=missing

# Build and test under AddressSanitizer + UBSan
asan: deps-asan
  uv run cmake --preset asan && uv run cmake --build --preset asan && uv run ctest --preset asan

# Build and test under ThreadSanitizer
tsan: deps-tsan
  uv run cmake --preset tsan && uv run cmake --build --preset tsan && uv run ctest --preset tsan

# --- Podman-first CI recipes (mirror .github/workflows/_required.yml) ---
# Each recipe builds/runs the CI suite inside the nestor-ci container image
# (ci/Containerfile) via scripts/run_ci_local.sh — podman by default, no
# native toolchain installs.

# Build the CI container image
ci-build:
  podman build -f ci/Containerfile -t nestor-ci:local .

# clang-format + yamllint + clang-tidy debug build (lint job)
ci-lint:
  ./scripts/run_ci_local.sh lint

# Build + labelled unit tests (unit-tests job)
ci-unit-tests:
  ./scripts/run_ci_local.sh unit

# Build + labelled integration tests (integration-tests job)
ci-integration-tests:
  ./scripts/run_ci_local.sh integration

# Build + 5x labelled concurrency tests (concurrency-tests job)
ci-concurrency-tests:
  ./scripts/run_ci_local.sh concurrency

# Build + live-NATS tests against a podman broker (nats-integration-tests job)
ci-nats-tests:
  ./scripts/run_ci_local.sh nats

# trivy fs + conan audit (security/dependency-scan job)
ci-security:
  ./scripts/run_ci_local.sh security

# gitleaks (security/secrets-scan job)
ci-secrets:
  ./scripts/run_ci_local.sh secrets

# Workflow schema validation + merge-queue policy tests (schema-validation job)
ci-schema:
  ./scripts/run_ci_local.sh schema

# CMake VERSION parse check (deps/version-sync job)
ci-deps-version-sync:
  ./scripts/run_ci_local.sh deps-version-sync

# Lockfile sync check (uv-check job)
ci-uv-check:
  ./scripts/run_ci_local.sh uv-check

# actionlint workflow lint
ci-actionlint:
  ./scripts/run_ci_local.sh actionlint

# Full podman-first CI suite
ci-all:
  ./scripts/run_ci_local.sh all
