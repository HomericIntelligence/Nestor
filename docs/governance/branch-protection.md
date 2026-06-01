# Branch Protection Policy for `main`

## Policy

All PRs to `main` require:

- **≥1 approving review** from a developer other than the author
- **All required status checks** must pass, including:
  - All Build/Test Checks
  - All Static Analysis Checks
  - All Coverage Checks
  - branch-protection-drift (verifies this configuration stays in sync)
- **Stale reviews are dismissed** — if a PR is updated after approval, a new review is required

## Rationale

- **Peer review**: Prevents self-merged code from entering the production service
- **Drift detection**: The `branch-protection-drift` check runs on every PR and ensures the live
  protection settings on GitHub match the canonical JSON in
  `.github/branch-protection/main.json`. If anyone modifies the protection rules via the GitHub
  UI, the next PR will fail the drift check and merge is blocked until the settings are
  re-applied
- **Dismiss stale reviews**: Ensures reviewers re-check changes after significant PR updates

## Configuration

The authoritative branch protection configuration is stored in
`.github/branch-protection/main.json`. This is the single source of truth.

Emergency hotfixes follow a different procedure (see below). In all normal cases, the protection
rules are not modified via the GitHub UI; they are defined in the JSON and applied via script.

## Applying Changes to Branch Protection

To modify the branch protection settings:

1. Edit `.github/branch-protection/main.json` with the desired changes
2. Create a PR with your changes
3. Get at least one approving review
4. **Before merging**, have an administrator run:

   ```bash
   bash scripts/apply-branch-protection.sh
   ```

   This applies the new settings to the live branch protection on GitHub.

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
   canonical settings from `.github/branch-protection/main.json`

The `enforce_admins` setting is kept `false` to permit this escape hatch. In normal operation,
this setting is never used.

## Verifying the Configuration

To verify that the live settings match the canonical JSON:

```bash
bash scripts/verify-branch-protection.sh
```

This exits with code 0 if the settings are correct, code 1 if there is drift. It is run
automatically on every PR via the `branch-protection-drift` job in
`.github/workflows/_required.yml`.
