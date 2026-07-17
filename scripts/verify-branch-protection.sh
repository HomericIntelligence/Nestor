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

ROOT="$(git rev-parse --show-toplevel)"
POLICY="${ROOT}/configs/github/merge-queue-policy.json"

if ! jq -e '
  (.repository | type == "string" and length > 0)
  and (.target_branch | type == "string" and length > 0)
  and (.activation_ruleset | type == "string" and length > 0)
  and (.rulesets | type == "array" and length > 0)
  and (all(.rulesets[];
    (.name | type == "string" and length > 0)
    and (.target | type == "string" and length > 0)
    and (.enforcement | type == "string" and length > 0)
    and (.source_type | type == "string" and length > 0)
    and (.source | type == "string" and length > 0)
    and (.conditions | type == "object")
    and (.conditions.ref_name | type == "object")
    and (.conditions.ref_name.include | type == "array" and length > 0)
    and (all(.conditions.ref_name.include[]; type == "string" and length > 0))
    and (.conditions.ref_name.exclude | type == "array")
    and (all(.conditions.ref_name.exclude[]; type == "string" and length > 0))
  ))
  and (([.rulesets[].name] | length)
    == ([.rulesets[].name] | unique | length))
  and (.activation_ruleset as $activation
    | ([.rulesets[].name] | index($activation) != null))
  and (([.rulesets[].name] | sort)
    == ([.required_checks[].authority] | unique | sort))
  and (.required_checks | type == "array" and length > 0)
  and (all(.required_checks[];
    (.context | type == "string" and length > 0)
    and (.workflow | type == "string" and length > 0)
    and (.authority | type == "string" and length > 0)))
  and (([.required_checks[].context] | length)
    == ([.required_checks[].context] | unique | length))
' "${POLICY}" >/dev/null; then
  echo "ERROR: invalid or duplicate required context in ${POLICY}"
  exit 1
fi

REPO="$(jq -r '.repository' "${POLICY}")"
BRANCH="$(jq -r '.target_branch' "${POLICY}")"
EXPECTED_ALL="$(jq -c '[.required_checks[].context] | sort' "${POLICY}")"

# Fetch the effective branch rules for main. This returns a flat array of the
# rules that apply to the branch (from branch protection and/or rulesets).
rules=$(gh api "repos/${REPO}/rules/branches/${BRANCH}")

# Helper: extract the parameters object for a given rule type.
params_for() {
  jq -c --arg t "$1" '[.[] | select(.type == $t)] | first | .parameters // empty' <<<"$rules"
}

pr_params=$(params_for "pull_request")

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

# Compare the complete effective context list, preserving duplicates so that
# overlap between rulesets cannot hide behind a set comparison.
LIVE_ALL="$(jq -c '
  [.[] | select(.type == "required_status_checks")
   | .parameters.required_status_checks[]?.context] | sort
' <<<"${rules}")"
if [ "${LIVE_ALL}" != "${EXPECTED_ALL}" ]; then
  echo "ERROR: effective required contexts differ from ${POLICY}"
  echo "expected: ${EXPECTED_ALL}"
  echo "live:     ${LIVE_ALL}"
  exit 1
fi

# Verify each named ruleset owns exactly the contexts assigned to it by the
# policy. The effective branch endpoint alone cannot detect a context moved
# from one protection layer to another.
rulesets=$(gh api "repos/${REPO}/rulesets?includes_parents=true")
while IFS= read -r authority; do
  match_count=$(jq -r --arg authority "${authority}" \
    '[.[] | select(.name == $authority)] | length' <<<"${rulesets}")
  if [ "${match_count}" -ne 1 ]; then
    echo "ERROR: expected exactly one live ruleset named ${authority}; found ${match_count}"
    exit 1
  fi

  ruleset_id=$(jq -r --arg authority "${authority}" \
    '.[] | select(.name == $authority) | .id' <<<"${rulesets}")
  ruleset=$(gh api "repos/${REPO}/rulesets/${ruleset_id}")
  expected_identity=$(jq -S -c --arg authority "${authority}" '
    .rulesets[] | select(.name == $authority)
    | {target, enforcement, source_type, source}
  ' "${POLICY}")
  live_identity=$(jq -S -c '{target, enforcement, source_type, source}' <<<"${ruleset}")
  if [ "${live_identity}" != "${expected_identity}" ]; then
    echo "ERROR: ruleset identity differs for ${authority}"
    echo "expected: ${expected_identity}"
    echo "live:     ${live_identity}"
    exit 1
  fi

  expected_conditions=$(jq -S -c --arg authority "${authority}" '
    .rulesets[] | select(.name == $authority) | .conditions
  ' "${POLICY}")
  live_conditions=$(jq -S -c '.conditions' <<<"${ruleset}")
  if [ "${live_conditions}" != "${expected_conditions}" ]; then
    echo "ERROR: branch conditions differ for ruleset ${authority}"
    echo "expected: ${expected_conditions}"
    echo "live:     ${live_conditions}"
    exit 1
  fi

  expected_split=$(jq -c --arg authority "${authority}" '
    [.required_checks[] | select(.authority == $authority) | .context] | sort
  ' "${POLICY}")
  live_split=$(jq -c '
    [.rules[] | select(.type == "required_status_checks")
     | .parameters.required_status_checks[]?.context] | sort
  ' <<<"${ruleset}")

  if [ "${live_split}" != "${expected_split}" ]; then
    echo "ERROR: required contexts differ for ruleset ${authority}"
    echo "expected: ${expected_split}"
    echo "live:     ${live_split}"
    exit 1
  fi
done < <(jq -r '[.required_checks[].authority] | unique[]' "${POLICY}")

echo "branch-protection-drift: OK"
