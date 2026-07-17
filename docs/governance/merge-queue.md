# Merge queue staged rollout

Nestor's required workflows are ready to report checks for GitHub
`merge_group` / `checks_requested` events. This readiness change does not
activate the queue. Independent human workflow review is an external gate, and
issue #128 stays open until an operator activates the queue and records a
representative queued smoke result.

[`configs/github/merge-queue-policy.json`](../../configs/github/merge-queue-policy.json)
is the sole machine-readable policy authority for the exact required contexts,
their posting workflows and protection layers, and the approved queue rule.
The legacy `.github/branch-protection/main.json` context list is a tested
compatibility projection, not another authority. Inspect the policy with:

```bash
POLICY=configs/github/merge-queue-policy.json
jq -r '.required_checks[] | [.context, .workflow, .authority] | @tsv' "${POLICY}"
jq '.merge_queue_rule' "${POLICY}"
```

The existing `push` and `pull_request` triggers, workflow permissions, security
jobs, and check names remain unchanged. Docker publication remains push-only;
merge-group runs build and validate the required `package`, `install`, and
`release` checks without logging into GHCR or pushing an image.

## Post-merge activation

Only Odysseus may authorize this operator step. Do not run it until this
readiness PR is merged, a smoke PR is designated, and a human has reviewed the
workflow and generated JSON payload.

First snapshot the complete repository ruleset, verify both protection layers
still expose the policy's exact contexts, and generate an append-only payload:

```bash
set -euo pipefail
REPO=HomericIntelligence/Nestor
POLICY=configs/github/merge-queue-policy.json
RULESET_NAME="$(jq -r '.activation_ruleset' "${POLICY}")"
RULESET_ID="$(gh api "repos/${REPO}/rulesets" \
  --jq ".[] | select(.name == \"${RULESET_NAME}\") | .id")"
test -n "${RULESET_ID}"

bash scripts/verify-branch-protection.sh

gh api "repos/${REPO}/rulesets/${RULESET_ID}" \
  | jq '{name, target, enforcement, bypass_actors, conditions, rules}' \
  > /tmp/nestor-ruleset-before.json

jq -e '[.rules[] | select(.type == "merge_queue")] | length == 0' \
  /tmp/nestor-ruleset-before.json

jq --slurpfile policy "${POLICY}" -e '
  ([.rules[] | select(.type == "required_status_checks")
    | .parameters.required_status_checks[].context] | sort)
  == ([$policy[0].required_checks[]
       | select(.authority == $policy[0].activation_ruleset)
       | .context] | sort)
' /tmp/nestor-ruleset-before.json

jq --slurpfile policy "${POLICY}" \
  '.rules += [$policy[0].merge_queue_rule]' \
  /tmp/nestor-ruleset-before.json > /tmp/nestor-ruleset-with-queue.json
```

Review `/tmp/nestor-ruleset-before.json` and
`/tmp/nestor-ruleset-with-queue.json` field by field. The latter may differ only
by the appended `merge_queue` rule. After Odysseus authorizes activation:

```bash
gh api --method PUT "repos/${REPO}/rulesets/${RULESET_ID}" \
  --input /tmp/nestor-ruleset-with-queue.json

gh api "repos/${REPO}/rulesets/${RULESET_ID}" \
  | jq --slurpfile policy "${POLICY}" -e '
      [.rules[] | select(.type == "merge_queue")]
      == [$policy[0].merge_queue_rule]
    '
```

## Queued smoke

Queue the designated PR with squash, obtain that PR's queue-head SHA, and prove
that a `merge_group` run and every policy check belong to that exact SHA. Do not
accept a successful pull-request-head run as queue evidence.

```bash
SMOKE_PR=123  # Replace only after Odysseus designates the smoke PR.
OWNER="${REPO%%/*}"
NAME="${REPO#*/}"
gh pr merge "${SMOKE_PR}" --repo "${REPO}" --auto --squash

QUEUE_ENTRY="$(gh api graphql \
  -f owner="${OWNER}" -f name="${NAME}" -F number="${SMOKE_PR}" \
  -f query='
    query($owner: String!, $name: String!, $number: Int!) {
      repository(owner: $owner, name: $name) {
        pullRequest(number: $number) {
          mergeQueueEntry { enqueuedAt headCommit { oid } }
        }
      }
    }
  ' --jq '.data.repository.pullRequest.mergeQueueEntry')"
ENQUEUED_AT="$(jq -r '.enqueuedAt' <<<"${QUEUE_ENTRY}")"
QUEUE_HEAD_SHA="$(jq -r '.headCommit.oid' <<<"${QUEUE_ENTRY}")"
test -n "${ENQUEUED_AT}" && test -n "${QUEUE_HEAD_SHA}"

RUNS="$(gh api --method GET "repos/${REPO}/actions/runs" \
  -f event=merge_group -f head_sha="${QUEUE_HEAD_SHA}" -f per_page=100)"
jq -e --arg sha "${QUEUE_HEAD_SHA}" '
  [.workflow_runs[] | select(.event == "merge_group" and .head_sha == $sha)]
  | length >= 1
' <<<"${RUNS}"

EXPECTED="$(jq -c '[.required_checks[].context] | sort' "${POLICY}")"
CHECKS="$(gh api --paginate \
  "repos/${REPO}/commits/${QUEUE_HEAD_SHA}/check-runs?filter=latest&per_page=100" \
  --jq '.check_runs[]' | jq -s '.')"
EMITTED="$(jq -c --argjson expected "${EXPECTED}" '
  [.[] | select(.name as $name | $expected | index($name)) | .name] | sort
' <<<"${CHECKS}")"
test "${EMITTED}" = "${EXPECTED}"
jq -e --argjson expected "${EXPECTED}" --arg sha "${QUEUE_HEAD_SHA}" '
  [.[] | select(.name as $name | $expected | index($name))]
  | length == ($expected | length)
    and all(.[]; .head_sha == $sha
      and .status == "completed" and .conclusion == "success")
' <<<"${CHECKS}"
```

Record the live ruleset response, merge-group run URL, exact check-run evidence,
and queued merge result in issue #128.

## Rollback

If activation changes any unrelated rule or the queued smoke fails, stop and
restore the reviewed snapshot:

```bash
gh api --method PUT "repos/${REPO}/rulesets/${RULESET_ID}" \
  --input /tmp/nestor-ruleset-before.json
gh api "repos/${REPO}/rulesets/${RULESET_ID}" \
  --jq '[.rules[] | select(.type == "merge_queue")] | length'
```

The final command must print `0`; re-run the exact required-context preflight
before resuming normal merges.
