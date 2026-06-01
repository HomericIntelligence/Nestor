#!/usr/bin/env bash
# Verify that main branch protection matches the HomericIntelligence ecosystem standard.
#
# This reads the *effective* branch rules via the
# `GET /repos/{owner}/{repo}/rules/branches/{branch}` endpoint, which is
# readable with the default Actions `contents: read` token. The previous
# implementation called `branches/{branch}/protection`, which requires an
# admin-scoped token the default GITHUB_TOKEN cannot obtain (resulting in a
# hard 403 "Resource not accessible by integration" on every PR). The rules
# endpoint exposes the same enforced invariants without needing admin scope.
#
# The asserted invariants match the ecosystem standard shared by all
# HomericIntelligence repos (Agamemnon, Keystone, Hermes, Scylla, Odysseus,
# Argus, Hephaestus, Myrmidons): PRs required, 0 required approvals,
# conversation-thread resolution required, required status checks enforced.
set -euo pipefail

REPO="HomericIntelligence/ProjectNestor"

# Fetch the effective branch rules for main. This returns a flat array of the
# rules that apply to the branch (from branch protection and/or rulesets).
rules=$(gh api "repos/${REPO}/rules/branches/main")

# Helper: extract the parameters object for a given rule type.
params_for() {
  jq -c --arg t "$1" '[.[] | select(.type == $t)] | first | .parameters // empty' <<<"$rules"
}

pr_params=$(params_for "pull_request")
checks_params=$(params_for "required_status_checks")

# A pull_request rule must be enforced (PRs required to merge to main).
if [ -z "$pr_params" ]; then
  echo "ERROR: no enforced pull_request rule on main"
  exit 1
fi

# Ecosystem standard: 0 required approvals. Assert the field is present and
# exactly 0 so drift in either direction (a stray required reviewer, or the
# rule being dropped) is caught.
if ! jq -e '.required_approving_review_count == 0' <<<"$pr_params" >/dev/null 2>&1; then
  echo "ERROR: required_approving_review_count must be 0 (ecosystem standard)"
  exit 1
fi

# Ecosystem standard: conversation threads must be resolved before merge.
if ! jq -e '.required_review_thread_resolution == true' <<<"$pr_params" >/dev/null 2>&1; then
  echo "ERROR: required_review_thread_resolution must be true"
  exit 1
fi

# A required_status_checks rule must be enforced so CI gates cannot be
# bypassed by merging a red PR.
if [ -z "$checks_params" ]; then
  echo "ERROR: no enforced required_status_checks rule on main"
  exit 1
fi

if ! jq -e '(.required_status_checks | length) >= 1' <<<"$checks_params" >/dev/null 2>&1; then
  echo "ERROR: required_status_checks must enforce at least one check"
  exit 1
fi

echo "branch-protection-drift: OK"
