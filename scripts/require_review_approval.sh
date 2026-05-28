#!/usr/bin/env bash
set -euo pipefail

readonly RULESET_ID="15556496"
readonly RULESET_NAME="homeric-main-baseline"
readonly REPO="HomericIntelligence/ProjectNestor"
readonly SNAPSHOT="/tmp/ruleset-before.json"
readonly PATCH="/tmp/ruleset-patch.json"
readonly AFTER="/tmp/ruleset-after.json"

# 1. Snapshot the live ruleset
echo "[1/9] Snapshotting live ruleset..."
gh api "repos/${REPO}/rulesets/${RULESET_ID}" > "$SNAPSHOT"

# 2. Assert name matches expected
echo "[2/9] Asserting ruleset name..."
actual_name=$(jq -r '.name' "$SNAPSHOT")
if [[ "$actual_name" != "$RULESET_NAME" ]]; then
  echo "ERROR: Expected ruleset name '$RULESET_NAME' but got '$actual_name'" >&2
  exit 1
fi

# 3. Build desired-state body via jq transform
echo "[3/9] Building patch body..."
jq '
  .rules |= map(
    if .type == "pull_request" then
      .parameters.required_approving_review_count = 1
      | .parameters.dismiss_stale_reviews_on_push = true
      | .parameters.require_last_push_approval = true
    else . end
  )
  | {name, target, enforcement, conditions, rules, bypass_actors}
' "$SNAPSHOT" > "$PATCH"

# 4. Validate patch is well-formed JSON and exactly three fields differ
echo "[4/9] Validating patch structure..."
jq empty "$PATCH" || {
  echo "ERROR: Patch JSON is malformed" >&2
  exit 1
}

# Note: Check target values but allow idempotent runs where snapshot == patch

# Verify the three specific values are correct
required_count=$(jq '.rules[] | select(.type=="pull_request").parameters.required_approving_review_count' "$PATCH")
dismiss_stale=$(jq '.rules[] | select(.type=="pull_request").parameters.dismiss_stale_reviews_on_push' "$PATCH")
require_push=$(jq '.rules[] | select(.type=="pull_request").parameters.require_last_push_approval' "$PATCH")

if [[ "$required_count" != "1" ]] || [[ "$dismiss_stale" != "true" ]] || [[ "$require_push" != "true" ]]; then
  echo "ERROR: Patch does not have expected values (count=$required_count, stale=$dismiss_stale, push=$require_push)" >&2
  exit 1
fi

# 5. Dry-run mode: print patch and exit
if [[ "${DRY_RUN:-}" == "1" ]]; then
  echo "[5/9] DRY_RUN mode: printing patch and exiting"
  cat "$PATCH"
  exit 0
fi

# 6. Noop-roundtrip: validate body shape by putting snapshot back unchanged
echo "[5/9] Performing noop-roundtrip validation..."
jq '{name, target, enforcement, conditions, rules, bypass_actors}' "$SNAPSHOT" | \
  gh api -X PUT "repos/${REPO}/rulesets/${RULESET_ID}" --input - > /dev/null 2>&1 || {
  echo "ERROR: Noop-roundtrip validation failed (API rejected body shape)" >&2
  echo "This suggests the body projection is incomplete or has incorrect field types." >&2
  exit 1
}

# 7. Apply the real change
echo "[6/9] Applying patch to live ruleset..."
gh api -X PUT "repos/${REPO}/rulesets/${RULESET_ID}" --input "$PATCH" > "$AFTER" || {
  status=$?
  echo "ERROR: PATCH request failed with status $status" >&2
  echo "ROLLBACK COMMAND:" >&2
  echo "jq '{name, target, enforcement, conditions, rules, bypass_actors}' $SNAPSHOT | \\" >&2
  echo "  gh api -X PUT repos/${REPO}/rulesets/${RULESET_ID} --input -" >&2
  exit "$status"
}

# 8. Assert all three target fields in after-state match desired values
echo "[7/9] Asserting post-patch state..."
after_count=$(jq '.rules[] | select(.type=="pull_request").parameters.required_approving_review_count' "$AFTER")
after_stale=$(jq '.rules[] | select(.type=="pull_request").parameters.dismiss_stale_reviews_on_push' "$AFTER")
after_push=$(jq '.rules[] | select(.type=="pull_request").parameters.require_last_push_approval' "$AFTER")

if [[ "$after_count" != "1" ]] || [[ "$after_stale" != "true" ]] || [[ "$after_push" != "true" ]]; then
  echo "ERROR: Post-patch values do not match expectations (count=$after_count, stale=$after_stale, push=$after_push)" >&2
  echo "ROLLBACK COMMAND:" >&2
  echo "jq '{name, target, enforcement, conditions, rules, bypass_actors}' $SNAPSHOT | \\" >&2
  echo "  gh api -X PUT repos/${REPO}/rulesets/${RULESET_ID} --input -" >&2
  exit 1
fi

# Assert rule-type set unchanged
rule_types_before=$(jq '.rules | map(.type) | sort' "$SNAPSHOT")
rule_types_after=$(jq '.rules | map(.type) | sort' "$AFTER")
if [[ "$rule_types_before" != "$rule_types_after" ]]; then
  echo "ERROR: Rule types changed" >&2
  exit 1
fi

# Assert required_status_checks count still 8
status_check_count=$(jq '.rules[] | select(.type=="required_status_checks").parameters.required_status_checks | length' "$AFTER")
if [[ "$status_check_count" != "8" ]]; then
  echo "ERROR: Required status checks count is $status_check_count, expected 8" >&2
  exit 1
fi

# 9. Diff snapshot vs after on the three target fields and print result
echo "[8/9] Comparing before and after state..."
if diff <(jq '.rules[] | select(.type=="pull_request").parameters | {required_approving_review_count, dismiss_stale_reviews_on_push, require_last_push_approval}' "$SNAPSHOT") \
        <(jq '.rules[] | select(.type=="pull_request").parameters | {required_approving_review_count, dismiss_stale_reviews_on_push, require_last_push_approval}' "$AFTER") > /dev/null 2>&1; then
  echo "NOOP"
else
  echo "CHANGED"
fi

# 10. Success
echo "[9/9] Script completed successfully"
