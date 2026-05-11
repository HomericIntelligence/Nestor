#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [ ! -f "${ROOT_DIR}/build/debug/compile_commands.json" ]; then
  cmake --preset debug -DProjectNestor_ENABLE_CLANG_TIDY=OFF >/dev/null
fi
find "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/test" \
  -name "*.cpp" -o -name "*.hpp" | \
  xargs clang-tidy -p "${ROOT_DIR}/build/debug" --config-file="${ROOT_DIR}/.clang-tidy" "$@"
