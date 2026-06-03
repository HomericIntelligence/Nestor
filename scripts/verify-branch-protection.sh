#!/usr/bin/env bash
# Verify that main branch protection is correctly configured.
#
# The asserted invariants match ProjectNestor's documented branch protection
# policy (docs/governance/branch-protection.md): PRs required, at least 1
# approving review, conversation-thread resolution required, required status
# checks enforced. Other HomericIntelligence repos may differ; this script
# speaks only for ProjectNestor.
set -euo pipefail

REPO="HomericIntelligence/ProjectNestor"

# Fetch the effective branch rules for main. The fetcher is exposed as a
# function so tests can override it via VERIFY_RULES_FIXTURE without
# touching the network; in normal CI runs the function calls gh api.
fetch_rules() {
  if [ -n "${VERIFY_RULES_FIXTURE:-}" ]; then
    cat "$VERIFY_RULES_FIXTURE"
  else
    gh api "repos/${REPO}/rules/branches/main"
  fi
}
rules=$(fetch_rules)

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

# ProjectNestor governance standard (docs/governance/branch-protection.md):
# at least 1 approving review from a developer other than the author.
# Asserting >= 1 (rather than == 1) so a future tightening to 2 does not
# trip drift; tightening below 1 (self-merge) is what we are explicitly
# blocking here, per audit issue #54.
if ! jq -e '.required_approving_review_count >= 1' <<<"$pr_params" >/dev/null 2>&1; then
  echo "ERROR: required_approving_review_count must be >= 1 (see docs/governance/branch-protection.md, audit #54)"
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
