#!/usr/bin/env bash
set -euo pipefail

# Explicit cached-dependency build. This wrapper never downloads dependencies.
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
case "${1:-test}" in
  configure)
    shift
    cmake -S "$root/test/fleet" -B "$root/build/fleet" "$@"
    ;;
  build)
    shift
    cmake --build "$root/build/fleet" --parallel 2 "$@"
    ;;
  test)
    shift
    ctest --test-dir "$root/build/fleet" --output-on-failure "$@"
    ;;
  *)
    echo 'usage: fleet-test.sh configure|build|test [arguments]' >&2
    exit 2
    ;;
esac
