# Pull Request

## Summary

One or two sentences explaining what this PR does and why.

## Linked issues

- Closes #
- Refs #

## Type of change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds capability)
- [ ] Breaking change (fix or feature that would cause existing behaviour to change)
- [ ] Documentation only

## Checklist

- [ ] Code builds with `just build` (or `cmake --build --preset debug`).
- [ ] `ctest --preset debug` passes locally.
- [ ] New conditional branches have test coverage.
- [ ] Public API / NATS subject changes documented in `README.md` or `AGENTS.md`.
- [ ] `CHANGELOG.md` updated (if user-visible).
- [ ] `pre-commit` / `just format-check` clean.

## Risk and rollout

- Backwards compatible: yes / no
- Requires config changes: yes / no
- Requires data migration: yes / no

## Additional notes

Anything reviewers should know — perf considerations, surprising design
choices, follow-up issues.
