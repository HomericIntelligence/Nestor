# Task Plan

## Goal

Produce a repository-grounded implementation plan for GitHub issue #130 without changing GitHub state.

## Phases

- [x] Read the authoritative project and planning-skill instructions.
- [x] Inventory TLS configuration types, consumers, tests, and build/test wrappers.
- [x] Determine the exact path-validation policy, implementation changes, and acceptance checks.
- [x] Draft the complete issue-ready implementation plan.

## Key Questions

- How are certificate and key paths represented and consumed after `TlsConfig::from_env`?
- Does the repository already establish an allowed TLS/config directory or filesystem-validation pattern?
- Which failure modes (relative paths, `..`, symlinks, non-regular files, unreadable files) must be covered?
- Which focused test filters and repository wrappers provide runnable acceptance evidence?

## Decisions

- Preserve the existing deployment contract: certificate and key environment variables may name files anywhere the process operator can access; do not add an undocumented base-directory variable or hard-coded directory.
- Normalize each non-empty input with `std::filesystem::weakly_canonical` using the `std::error_code` overload, require the resolved target to be a regular file, probe readability only through the resolved path, and return the resolved path to `SSLServer`.
- Add a narrowly scoped `// codeql[cpp/path-injection]` suppression at the remaining `ifstream` sink because CodeQL intentionally treats all environment variables as tainted while this API treats them as trusted process-operator configuration. Keep the runtime validation even though the trust boundary is documented.
- Extend `test/src/test_tls_config.cpp`; do not create a redundant test target or file.

## Errors Encountered

| Error | Attempt | Resolution |
| --- | --- | --- |
| `zsh: no matches found: docker*` while searching documentation | 1 | Re-run the search from repository root with `rg` include/exclude globs instead of a shell-expanded positional glob. |
| `./scripts/test.sh -R '^TlsConfigTest\.'` failed because `cmake` is not installed in the planning environment | 1 | Do not retry locally; retain the repository wrapper as the implementation-time verification command and report the prerequisite. |
