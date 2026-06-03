#!/usr/bin/env bash
# Apply branch protection settings to the main branch, then read the live
# state back and verify the required_approving_review_count field actually
# took effect. Without this read-back, a silently-ignored field in the PUT
# body would leave the live setting unchanged with no error (see audit #54).
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
REPO="HomericIntelligence/ProjectNestor"
CONFIG="${ROOT}/.github/branch-protection/main.json"

expected_count=$(jq -r '.required_pull_request_reviews.required_approving_review_count' "$CONFIG")

echo "Applying branch protection from ${CONFIG} ..."
gh api --method PUT "repos/${REPO}/branches/main/protection" --input "$CONFIG" >/dev/null

echo "Reading live protection back to verify required_approving_review_count..."
live_count=$(gh api "repos/${REPO}/branches/main/protection" \
  --jq '.required_pull_request_reviews.required_approving_review_count // 0')

if [ "$live_count" != "$expected_count" ]; then
  echo "ERROR: applied required_approving_review_count=${expected_count}" \
       "but live state reports ${live_count}; PUT was silently rejected or ignored." >&2
  exit 1
fi

echo "OK: live required_approving_review_count = ${live_count}"
