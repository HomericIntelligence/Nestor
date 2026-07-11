#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

IMAGE_NAME="nestor-clang-format"
DOCKERFILE="${ROOT_DIR}/Dockerfile.clang-format"

# Build the container image if it doesn't exist
if ! podman image exists "${IMAGE_NAME}" 2>/dev/null; then
  echo "Building clang-format container..."
  podman build -t "${IMAGE_NAME}" -f "${DOCKERFILE}" "${ROOT_DIR}"
fi

CHECK_MODE=""
if [[ "${1:-}" == "--check" ]]; then CHECK_MODE="--dry-run --Werror"; fi

# Collect files relative to ROOT_DIR for the container
FILES=()
while IFS= read -r -d '' f; do
  FILES+=("/src/${f#"${ROOT_DIR}/"}")
done < <(find "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/test" \
  \( -name "*.cpp" -o -name "*.hpp" \) -print0)

# Run clang-format inside the container with source mounted at /src
# shellcheck disable=SC2086
podman run --rm -v "${ROOT_DIR}:/src:z" "${IMAGE_NAME}" \
  --style=file ${CHECK_MODE} -i "${FILES[@]}"
