#!/usr/bin/env bash
# Verify that main branch protection is correctly configured.
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"

# Fetch the live branch protection for the main branch
live=$(gh api repos/HomericIntelligence/ProjectNestor/branches/main/protection)

# Check required_approving_review_count >= 1
if ! jq -e '.required_pull_request_reviews.required_approving_review_count >= 1' <<<"$live" >/dev/null 2>&1; then
  echo "ERROR: required_approving_review_count must be >= 1"
  exit 1
fi

# Check dismiss_stale_reviews == true
if ! jq -e '.required_pull_request_reviews.dismiss_stale_reviews == true' <<<"$live" >/dev/null 2>&1; then
  echo "ERROR: dismiss_stale_reviews must be true"
  exit 1
fi

# Check that branch-protection-drift is in required_status_checks.contexts
if ! jq -e '.required_status_checks.contexts | index("branch-protection-drift") != null' <<<"$live" >/dev/null 2>&1; then
  echo "ERROR: branch-protection-drift must be in required_status_checks.contexts"
  exit 1
fi

echo "branch-protection-drift: OK"
