#!/usr/bin/env bash
# Apply branch protection settings to the main branch.
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"

gh api --method PUT "repos/HomericIntelligence/Nestor/branches/main/protection" --input "${ROOT}/.github/branch-protection/main.json"
