# Required checks on the merge queue

Nestor's `main` branch already requires the merge queue. Its live rule uses
`HEADGREEN`, two concurrent queue entries, at most five entries per merge, a
60-minute check timeout, and squash merging. The minimum merge size is one,
with a five-minute minimum-size wait. The checked-in snapshot records these
settings; changing that snapshot does not change GitHub protection.

[`configs/github/merge-queue-policy.json`](../../configs/github/merge-queue-policy.json)
maps all 16 required contexts to their workflow and ruleset owners. The legacy
`.github/branch-protection/main.json` context list remains a tested compatibility
projection. Both protection layers must retain their exact context sets.

| Workflow | Required contexts |
| --- | --- |
| `_required.yml` | `branch-protection-drift`, `build`, `deps/version-sync`, `integration-tests`, `lint`, `schema-validation`, `security/dependency-scan`, `security/secrets-scan`, `test`, `unit-tests` |
| `build-test.yml` | `All Build/Test Checks` |
| `static-analysis.yml` | `All Static Analysis Checks` |
| `code-coverage.yml` | `All Coverage Checks` |
| `docker-publish.yml` | `package`, `install`, `release` |

Each producer handles `merge_group` / `checks_requested` using its existing
jobs and dependencies. The aggregate jobs reject failure, cancellation and
skipped prerequisites. Docker login and image publication remain push-only;
queue runs build packages, perform the install smoke, and validate the release
without publishing. The former `merge-queue-smoke` workflow has been removed;
every required context must come from its normal producer. Workflow concurrency
separates PR runs by PR number and merge-group runs by their synthetic commit,
so unrelated PRs and queue runs do not cancel each other. CodeQL's separate
schedule and permissions remain unchanged.

## Verify and admit a reviewed PR

1. Complete the repository's current-source local CI, hosted CI and review
   requirements. Independent human workflow review remains an external
   governance gate; the live rules require zero approving reviews and resolved
   conversations. Local tests and green PR-head checks do not prove queue-head
   execution.
2. Verify the live rules without changing them:

   ```bash
   set -euo pipefail
   REPO=HomericIntelligence/Nestor
   POLICY=configs/github/merge-queue-policy.json
   bash scripts/verify-branch-protection.sh
   gh api "repos/${REPO}/rules/branches/main" > /tmp/nestor-queue-rules.json
   jq -r '.required_checks[] | [.context, .workflow, .authority] | @tsv' "${POLICY}"
   ```

   The drift check requires exactly one effective queue rule with the complete
   recorded parameters, plus the existing review invariants and exact required
   contexts and ruleset identities. Missing, duplicate or changed queue rules
   fail. Its fixture tests substitute only the external `gh` responses.
3. Admit only the reviewed PR head through the normal queue:

   ```bash
   PR_NUMBER=167  # Set to the reviewed PR.
   PR_HEAD="$(gh pr view "${PR_NUMBER}" --repo "${REPO}" --json headRefOid --jq .headRefOid)"
   gh pr merge "${PR_NUMBER}" --repo "${REPO}" --auto --squash \
     --match-head-commit "${PR_HEAD}"
   ```

4. Capture the queue entry while it exists and bind evidence to its synthetic
   head, not the PR head:

   ```bash
   QUEUE_ENTRY="$(gh api graphql \
     -f owner="${REPO%%/*}" -f name="${REPO#*/}" -F number="${PR_NUMBER}" \
     -f query='
       query($owner: String!, $name: String!, $number: Int!) {
         repository(owner: $owner, name: $name) {
           pullRequest(number: $number) {
             mergeQueueEntry { enqueuedAt headCommit { oid } }
           }
         }
       }
     ' --jq '.data.repository.pullRequest.mergeQueueEntry')"
   QUEUE_HEAD_SHA="$(jq -er '.headCommit.oid | select(type == "string" and length == 40)' <<<"${QUEUE_ENTRY}")"
   gh api --method GET "repos/${REPO}/actions/runs" \
     -f event=merge_group -f head_sha="${QUEUE_HEAD_SHA}" -f per_page=100 \
     > /tmp/nestor-queue-runs.json
   gh api --paginate \
     "repos/${REPO}/commits/${QUEUE_HEAD_SHA}/check-runs?filter=latest&per_page=100" \
     --jq '.check_runs[]' | jq -s '.' > /tmp/nestor-queue-checks.json
   ```

5. Require the genuine merge-group runs and all 16 successful contexts on that
   exact head. Inspect every workflow job, including unpinned jobs, when a run
   fails. The following check rejects missing, duplicate, stale, skipped and
   unsuccessful required results:

   ```bash
   jq -e --arg sha "${QUEUE_HEAD_SHA}" '
     any(.workflow_runs[]; .event == "merge_group" and .head_sha == $sha)
   ' /tmp/nestor-queue-runs.json
   jq --slurpfile policy "${POLICY}" --arg sha "${QUEUE_HEAD_SHA}" -e '
     ([$policy[0].required_checks[].context] | sort) as $expected
     | [.[] | select(.name as $name | $expected | index($name))] as $checks
     | ([$checks[].name] | sort) == $expected
       and all($checks[]; .head_sha == $sha and .status == "completed"
         and .conclusion == "success")
   ' /tmp/nestor-queue-checks.json
   ```

Record the rules, source head, queue head, actual run URLs, check results and
normal merge outcome with the PR. A trigger regression proves configuration
readiness only. Until GitHub discovers and executes every corrected producer on
the actual queue head, queue acceptance remains unverified. If only smoke runs
or checks time out, retain the failure and repair workflow execution. Do not
mirror statuses, bypass the queue, weaken required contexts, or remove its rule.
