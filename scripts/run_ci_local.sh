#!/bin/bash
# Run the Nestor CI suite locally inside a container.
#
# Mirrors what GitHub Actions runs, using the same CI container image
# (ci/Containerfile). Supports Podman (rootless, preferred) or Docker.
#
# Usage:
#   ./scripts/run_ci_local.sh                  # Run all CI checks
#   ./scripts/run_ci_local.sh lint             # clang-format + yamllint + clang-tidy (lint job)
#   ./scripts/run_ci_local.sh unit             # build + labelled unit ctest (unit-tests job)
#   ./scripts/run_ci_local.sh integration      # build + labelled integration ctest
#   ./scripts/run_ci_local.sh concurrency      # build + 5x labelled concurrency ctest
#   ./scripts/run_ci_local.sh nats             # build + live-NATS ctest against a podman broker
#   ./scripts/run_ci_local.sh security         # trivy fs + conan audit scans
#   ./scripts/run_ci_local.sh secrets          # gitleaks (security-secrets-scan job)
#   ./scripts/run_ci_local.sh schema           # workflow schema validation + merge-queue policy tests
#   ./scripts/run_ci_local.sh deps-version-sync  # CMake VERSION parse check
#   ./scripts/run_ci_local.sh uv-check         # lockfile sync check
#   ./scripts/run_ci_local.sh actionlint       # workflow lint
#
# Container engine: auto-detected (podman first, docker fallback).
# Override: CONTAINER_ENGINE=docker ./scripts/run_ci_local.sh
#
# Image: uses 'nestor-ci:local'.
# Build locally: just ci-build  (or: podman build -f ci/Containerfile -t nestor-ci:local .)
#
# Optional: CONAN_HOME_HOST=/path/to/.conan2 mounts a host conan cache at
# /home/ci/.conan2 inside the container (CI sets this to $HOME/.conan2).

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBSET="${1:-all}"

LOCAL_IMAGE="nestor-ci:local"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[CI]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[CI]${NC} $*"; }
log_error() { echo -e "${RED}[CI]${NC} $*" >&2; }
log_step()  { echo -e "\n${BLUE}==>${NC} $*"; }

# ============================================================================
# Container engine detection
# ============================================================================

detect_engine() {
    if [ -n "${CONTAINER_ENGINE:-}" ]; then
        if ! command -v "${CONTAINER_ENGINE}" &> /dev/null; then
            log_error "CONTAINER_ENGINE=${CONTAINER_ENGINE} not found in PATH"
            exit 1
        fi
        log_info "Container engine: ${CONTAINER_ENGINE} (from env)"
        return
    fi

    if command -v podman &> /dev/null; then
        CONTAINER_ENGINE="podman"
        log_info "Container engine: podman (rootless)"
    elif command -v docker &> /dev/null; then
        CONTAINER_ENGINE="docker"
        log_info "Container engine: docker"
    else
        log_error "No container engine found. Install podman (recommended) or docker."
        exit 1
    fi
    export CONTAINER_ENGINE
}

# ============================================================================
# Image resolution
# ============================================================================

resolve_image() {
    if "${CONTAINER_ENGINE}" image exists "${LOCAL_IMAGE}" 2>/dev/null || \
       "${CONTAINER_ENGINE}" images -q "${LOCAL_IMAGE}" 2>/dev/null | grep -q .; then
        CI_IMAGE="${LOCAL_IMAGE}"
        log_info "Using local CI image: ${CI_IMAGE}"
    else
        log_error "Local image '${LOCAL_IMAGE}' not found."
        log_error "Build it first: just ci-build"
        exit 1
    fi
    export CI_IMAGE
}

# ============================================================================
# Run a command inside the CI container (workspace mounted at /workspace)
# ============================================================================

run_in_container() {
    local cmd=("$@")
    local engine_flags=()

    if [ "${CONTAINER_ENGINE}" = "podman" ]; then
        # --userns=keep-id:uid=1000,gid=1000 maps the container's ci user (uid 1000) to the
        # invoking host user, so volume-mounted build artifacts keep the same
        # owner whether run locally or on a GitHub runner (uid 1001).
        engine_flags+=(--userns=keep-id:uid=1000,gid=1000)
    fi

    # Conan 2 writes its global.conf + cache to $CONAN_HOME on first run. On
    # GitHub runners the host conan dir is owned by a different uid than the
    # container's mapped user, so pointing CONAN_HOME at a container-local
    # path (/tmp) is always writable. The optional host mount below still
    # provides a cold-cache speedup when the host dir happens to be writable.
    engine_flags+=(-e CONAN_HOME=/tmp/conan2)
    engine_flags+=(-e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}")
    if [ -n "${CONAN_HOME_HOST:-}" ]; then
        engine_flags+=(-v "${CONAN_HOME_HOST}:/home/ci/.conan2:Z")
    fi
    # /workspace is a bind mount whose git metadata is owned by the host user;
    # mark it safe so `git` commands inside the container (merge-queue policy
    # tests, schema drift checks) do not fail with "dubious ownership".
    engine_flags+=(-e GIT_CONFIG_COUNT=1)
    engine_flags+=(-e GIT_CONFIG_KEY_0=safe.directory)
    engine_flags+=(-e GIT_CONFIG_VALUE_0=/workspace)

    if [ -n "${CONAN_AUDIT_PROVIDER_TOKEN:-}" ]; then
        engine_flags+=(-e CONAN_AUDIT_PROVIDER_TOKEN="${CONAN_AUDIT_PROVIDER_TOKEN}")
    fi

    "${CONTAINER_ENGINE}" run --rm \
        "${engine_flags[@]}" \
        --volume "${PROJECT_ROOT}:/workspace:Z" \
        --workdir /workspace \
        "${CI_IMAGE}" \
        "${cmd[@]}"
}

# ============================================================================
# CI steps (mirror .github/workflows/_required.yml)
# ============================================================================

run_lint() {
    log_step "lint: CI launcher contracts + clang-format + yamllint + clang-tidy debug build"
    run_in_container bash -c '
        set -euo pipefail
        PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test -p "test_ci_*.py" -v
        mapfile -t files < <(find src include -type f \( -name "*.cpp" -o -name "*.hpp" \))
        if [ "${#files[@]}" -gt 0 ]; then
            clang-format --dry-run --Werror "${files[@]}"
        fi
        yamllint -d relaxed .github/
        conan profile detect --exist-ok
        conan install . --build=missing -s build_type=Debug --output-folder build/debug
        cmake --preset debug -DNestor_ENABLE_CLANG_TIDY=ON
        cmake --build --preset debug
        mapfile -t cpp_files < <(find src -name "*.cpp" | head -5)
        if [ "${#cpp_files[@]}" -gt 0 ]; then
            clang-tidy -p build/debug \
              --extra-arg-before=-Wno-unknown-warning-option \
              "${cpp_files[@]}"
        else
            echo "WARN: no src/*.cpp files found to clang-tidy"
        fi
    '
}

run_build_release() {
    # Shared: conan install (Release) + configure + build. Emits build/release.
    # Existing generated files are incremental inputs, never proof that the
    # current source has been configured and built successfully.
    run_in_container bash -c '
        set -euo pipefail
        conan profile detect --exist-ok
        conan install . --build=missing -s build_type=Release --output-folder build/release
        cmake --preset release
        cmake --build --preset release
    '
}

run_unit() {
    log_step "unit-tests: build + labelled unit ctest"
    run_build_release
    run_in_container bash -c '
        set -euo pipefail
        cd build/release
        ctest --output-on-failure -L unit --no-tests=error
    '
}

run_integration() {
    log_step "integration-tests: build + labelled integration ctest"
    run_build_release
    run_in_container bash -c '
        set -euo pipefail
        cd build/release
        ctest --output-on-failure -L integration --no-tests=error
    '
}

run_concurrency() {
    log_step "concurrency-tests: build + 5x labelled concurrency ctest"
    run_build_release
    run_in_container bash -c '
        set -euo pipefail
        cd build/release
        for i in $(seq 5); do
            echo "--- concurrency run $i/5 ---"
            ctest --output-on-failure -L concurrency --no-tests=error
        done
    '
}

run_nats() (
    log_step "nats-integration-tests: build + private live-NATS ctest"
    run_build_release

    # Run on the Linux engine host. Private cidfiles bind cleanup to containers
    # created by this invocation, including a client failure after creation.
    local state_dir broker endpoint monitor socket_path
    state_dir="$(mktemp -d)"
    cleanup_nats() {
        local status=$? cleanup_status=0 id file
        trap - EXIT
        for file in "${state_dir}/test.cid" "${state_dir}/broker.cid"; do
            if [ -s "${file}" ]; then
                id="$(cat "${file}")"
                if [[ "${id}" =~ ^[a-f0-9]{64}$ ]]; then
                    if "${CONTAINER_ENGINE}" rm -f "${id}"; then
                        :
                    else
                        cleanup_status=1
                    fi
                else
                    log_error "Invalid owned container receipt: ${file}"
                    cleanup_status=1
                fi
            fi
        done
        if [ "${cleanup_status}" -eq 0 ]; then
            rm -rf -- "${state_dir}"
        else
            log_error "Container cleanup is uncertain; retained receipts: ${state_dir}"
        fi
        if [ "${status}" -eq 0 ]; then
            exit "${cleanup_status}"
        fi
        exit "${status}"
    }
    trap cleanup_nats EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    broker="$("${CONTAINER_ENGINE}" run -d --cidfile "${state_dir}/broker.cid" \
        -p 127.0.0.1::4222 -p 127.0.0.1::8222 \
        nats:2.12-alpine -js -m 8222)"
    if [[ ! "${broker}" =~ ^[a-f0-9]{64}$ ]]; then
        log_error "Broker creation returned no exact container identity"
        return 1
    fi
    endpoint="$("${CONTAINER_ENGINE}" port "${broker}" 4222/tcp)"
    monitor="$("${CONTAINER_ENGINE}" port "${broker}" 8222/tcp)"
    if [[ ! "${endpoint}" =~ ^127\.0\.0\.1:[0-9]+$ ]] || \
       [[ ! "${monitor}" =~ ^127\.0\.0\.1:[0-9]+$ ]]; then
        log_error "Broker port allocation was not private loopback"
        return 1
    fi
    local healthy=0
    for _ in $(seq 1 30); do
        if curl --connect-timeout 1 --max-time 1 -fsS "http://${monitor}/healthz" >/dev/null 2>&1; then
            healthy=1
            break
        fi
        sleep 1
    done
    if [ "${healthy}" -ne 1 ]; then
        log_error "NATS broker did not become healthy within the bounded wait"
        return 1
    fi

    # The C++ bounce tests use compose-shaped commands. This private shim
    # targets only the exact broker ID returned above, on the explicit socket.
    cat > "${state_dir}/docker" <<'SHIM'
#!/bin/bash
set -euo pipefail
if [ "$#" -eq 5 ] && [ "$1" = compose ] && [ "$2" = -f ] && [ "$5" = nats ]; then
    case "$4" in
        stop|start) exec /usr/bin/docker --host unix:///var/run/docker.sock "$4" "${NESTOR_LIVE_NATS_BROKER:?}" ;;
    esac
fi
echo "Unsupported private broker lifecycle command" >&2
exit 1
SHIM
    chmod 755 "${state_dir}/docker"
    local engine_flags=()
    if [ "${CONTAINER_ENGINE##*/}" = podman ]; then
        socket_path="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/podman/podman.sock"
        engine_flags+=(--userns=keep-id:uid=1000,gid=1000)
    else
        socket_path="${DOCKER_HOST:-unix:///var/run/docker.sock}"
        if [[ "${socket_path}" != unix:///* ]]; then
            log_error "Live NATS requires an explicit local Unix engine socket"
            return 1
        fi
        socket_path="${socket_path#unix://}"
    fi
    # timeout bounds the client; EXIT cleanup also removes its exact container
    # if the client disconnects while the tests remain active.
    timeout 900s "${CONTAINER_ENGINE}" run --cidfile "${state_dir}/test.cid" \
        "${engine_flags[@]}" --network host \
        -v "${PROJECT_ROOT}:/workspace:Z" \
        -v "${socket_path}:/var/run/docker.sock:Z" \
        -v "${state_dir}/docker:/usr/local/bin/docker:Z" \
        -e "NESTOR_LIVE_NATS_URL=nats://${endpoint}" \
        -e "NESTOR_LIVE_NATS_BROKER=${broker}" \
        -e NESTOR_LIVE_NATS_COMPOSE=/workspace/test/docker/docker-compose.nats.yml \
        -w /workspace "${CI_IMAGE}" \
        bash -c '
            set -euo pipefail
            cd build/release
            ctest --output-on-failure -L live-nats --no-tests=error
        '
)

run_security() {
    log_step "security/dependency-scan: trivy fs + conan audit"
    run_in_container bash -c '
        set -euo pipefail
        trivy fs --exit-code 0 --severity HIGH,CRITICAL --scanners vuln --skip-dirs build .
        conan profile detect --exist-ok
        if [ -n "${CONAN_AUDIT_PROVIDER_TOKEN:-}" ]; then
            conan audit scan .
        else
            echo "WARN: CONAN_AUDIT_PROVIDER_TOKEN unset — skipping conan audit scan"
        fi
    '
}

run_secrets() {
    log_step "security/secrets-scan: gitleaks"
    run_in_container bash -c '
        set -euo pipefail
        if [ -f .gitleaks.toml ]; then
            gitleaks detect --source . --config .gitleaks.toml \
              --report-format sarif --report-path gitleaks.sarif --exit-code 0
        else
            gitleaks detect --source . \
              --report-format sarif --report-path gitleaks.sarif --exit-code 0
        fi
    '
}

run_schema() {
    log_step "schema-validation: workflow schemas + merge-queue policy tests"
    run_in_container bash -c '
        set -euo pipefail
        find .github/workflows -name "*.yml" | \
            xargs check-jsonschema --builtin-schema vendor.github-workflows
        python3 test/test_merge_queue.py
    '
}

run_deps_version_sync() {
    log_step "deps/version-sync: CMake VERSION parseable"
    run_in_container python3 -c '
import re
cmake = open("CMakeLists.txt").read()
v = re.search(r"VERSION\s+(\S+)", cmake).group(1)
print(f"CMake version: {v}")
'
}

run_uv_check() {
    log_step "uv-check: lockfile sync"
    run_in_container bash -c '
        set -euo pipefail
        uv lock --check
    '
}

run_actionlint() {
    log_step "actionlint: workflow lint"
    run_in_container actionlint
}

# ============================================================================
# Main
# ============================================================================

FAILED=()

run_step() {
    local name="$1"
    local fn="$2"
    # Keep `set -e` active inside fn (fail-fast: a broken build must not let
    # follow-on steps run and emit confusing downstream errors), but capture
    # the exit code here so the top-level script survives to report all
    # failures.
    set +e
    (
        set -e
        "${fn}"
    )
    local rc=$?
    set -e
    if [ "${rc}" -ne 0 ]; then
        FAILED+=("${name}")
        log_error "${name} FAILED"
    fi
}

detect_engine
resolve_image

log_info "CI subset: ${SUBSET}"
log_info "Project root: ${PROJECT_ROOT}"

case "${SUBSET}" in
    lint)
        run_step "lint" run_lint
        ;;
    unit)
        run_step "unit-tests" run_unit
        ;;
    integration)
        run_step "integration-tests" run_integration
        ;;
    concurrency)
        run_step "concurrency-tests" run_concurrency
        ;;
    build)
        run_step "build" run_build_release
        ;;
    nats)
        run_step "nats-integration-tests" run_nats
        ;;
    security)
        run_step "security/dependency-scan" run_security
        ;;
    secrets)
        run_step "security/secrets-scan" run_secrets
        ;;
    schema)
        run_step "schema-validation" run_schema
        ;;
    deps-version-sync)
        run_step "deps/version-sync" run_deps_version_sync
        ;;
    uv-check)
        run_step "uv-check" run_uv_check
        ;;
    actionlint)
        run_step "actionlint" run_actionlint
        ;;
    all)
        run_step "lint" run_lint
        run_step "uv-check" run_uv_check
        run_step "build" run_build_release
        run_step "unit-tests" run_unit
        run_step "integration-tests" run_integration
        run_step "concurrency-tests" run_concurrency
        run_step "nats-integration-tests" run_nats
        run_step "actionlint" run_actionlint
        run_step "security/dependency-scan" run_security
        run_step "security/secrets-scan" run_secrets
        run_step "schema-validation" run_schema
        run_step "deps/version-sync" run_deps_version_sync
        ;;
    *)
        log_error "Unknown subset: ${SUBSET}"
        log_error "Valid values: all, lint, unit, integration, concurrency, nats, security, secrets, schema, deps-version-sync, uv-check, actionlint"
        exit 1
        ;;
esac

echo ""
if [ "${#FAILED[@]}" -eq 0 ]; then
    log_info "All CI checks passed."
else
    log_error "Failed: ${FAILED[*]}"
    exit 1
fi
