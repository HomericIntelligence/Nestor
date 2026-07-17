# Branch Protection Policy for `main`

## Policy

The live protection payload for `main` is a checks-only merge gate. All PRs to
`main` require:

- **No approving review** — `required_approving_review_count` is intentionally
  `0`
- **All required status checks** must pass, including:
  - All Build/Test Checks
  - All Static Analysis Checks
  - All Coverage Checks
  - branch-protection-drift (verifies this configuration stays in sync)
- **Review conversations must be resolved**
- **Stale reviews are not dismissed** — `dismiss_stale_reviews` is `false`

Independent human review of workflow changes is an external governance gate. It
must be completed before an operator activates the staged merge queue, but it is
not represented as an approving-review requirement in live branch protection.

## Rationale

- **Checks and conversation resolution**: Keep automated validation and review
  discussion closure as the live merge gates without inventing an approval rule
- **External workflow review**: Ensures a human independently reviews workflow
  changes before merge-queue activation; this is outside the live protection
  payload
- **Drift detection**: The `branch-protection-drift` check runs on every PR and compares the
  exact effective context set and each ruleset's context ownership with
  `configs/github/merge-queue-policy.json`. If anyone modifies required checks via the GitHub UI,
  the next PR fails until the live split and policy agree

## Configuration

[`configs/github/merge-queue-policy.json`](../../configs/github/merge-queue-policy.json) is the
sole machine-readable authority for required contexts, their workflow owners, and their ruleset
owners. The context list in `.github/branch-protection/main.json` is a compatibility projection
for the legacy branch-protection endpoint, and the regression suite requires that projection to
match the policy's `homeric-main-extras` split. It is not a second context authority.

`.github/branch-protection/main.json` remains the application payload for legacy review and
branch settings that are outside the required-context policy. Its review
settings intentionally require zero approvals and do not dismiss stale reviews.
The merge-queue readiness PR does not change this payload or live protection;
queue activation is a separate, post-merge operator step described in
[`merge-queue.md`](merge-queue.md).

Emergency hotfixes follow a different procedure (see below). In all normal cases, the protection
rules are not modified via the GitHub UI; they are defined in the JSON and applied via script.

## Applying Changes to Branch Protection

For the staged merge-queue rollout, edit the merge-queue policy only when the
required contexts or approved queue rule need to change, update the named
workflow job, and follow the reviewed procedure in the
[merge-queue runbook](merge-queue.md). The readiness PR does not require a
live-protection mutation. Independent human workflow review remains an external
gate before any post-merge activation.

To intentionally modify only the legacy review or branch settings:

1. Edit `.github/branch-protection/main.json` without treating its compatibility context list as
   an independent policy
2. Create a PR with your changes
3. Run the repository's documented checks and obtain any required external human
   review for the affected workflow or protection change
4. **Only for an approved protection change**, have an administrator run:

   ```bash
   bash scripts/apply-branch-protection.sh
   ```

   This applies the legacy endpoint settings to live branch protection on GitHub. It does not
   replace the required-context ruleset procedure.

5. The `branch-protection-drift` job on the PR will re-run (or request CI to re-run). Once it
   passes, the PR is ready to merge.
6. Merge the PR

## Emergency Hotfix Procedure

In rare cases where branch protection must be temporarily modified without committing the change:

1. An administrator with repo access can temporarily lower the rule via GitHub's web UI or API
2. This **must** be documented in a Slack #incidents channel or equivalent with:
   - Who made the change and when
   - Why it was necessary
   - When it will be restored
3. The PR that merges during the window **must** include a follow-up commit that restores the
   settings
4. After the PR merges, immediately run `bash scripts/apply-branch-protection.sh` to restore the
   legacy settings from `.github/branch-protection/main.json`, then run the drift check to verify
   the policy-owned context split

The `enforce_admins` setting is kept `false` to permit this escape hatch. In normal operation,
this setting is never used.

## Verifying the Configuration

To verify that the live settings match the canonical JSON:

```bash
bash scripts/verify-branch-protection.sh
```

This exits with code 0 only when the review invariants, complete effective context set, and exact
per-ruleset split match policy. It is run automatically on every PR via the
`branch-protection-drift` job in
`.github/workflows/_required.yml`.
