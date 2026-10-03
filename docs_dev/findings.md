# Findings

- Issue #130 requires canonicalizing environment-derived TLS certificate/key paths before validation/opening, validating regular files, and preventing surprising path traversal or symlink behavior when a base directory is enforced.
- `src/tls_config.cpp` currently assigns both environment strings directly to `TlsConfig` and probes them with `std::ifstream`.
- `test/src/test_tls_config.cpp` is already part of the single `${PROJECT_NAME}_tests` target in `test/CMakeLists.txt`; focused TLS tests can be added there without creating or registering a new test file.
- Project instructions require C++20 and normal build/test execution through `scripts/` wrappers.
- `TlsConfig` stores paths as strings (`include/nestor/tls_config.hpp:17-18`), and `src/server_main.cpp:173` passes the successful loader output directly to `httplib::SSLServer`; canonicalized paths should therefore replace the raw environment strings in the returned config.
- No existing filesystem canonicalization, regular-file validation, allowed TLS directory, or config-directory environment variable was found in the first repository-wide source search.
- `scripts/test.sh` configures and builds the debug preset, then forwards arguments to `ctest --preset debug --output-on-failure`; focused verification can use `./scripts/test.sh -R <regex>`.
- Repository-wide documentation/config search found no established TLS base directory, `/etc/nestor` layout, or environment variable for an allowed config directory. Imposing one would be a new deployment contract, so the plan should canonicalize and validate the operator-supplied paths without inventing a base-directory restriction.
- `.github/workflows/codeql.yml:33` runs the `security-and-quality` query suite against first-party code, so the authoritative recurrence check remains the existing `CodeQL (cpp)` CI job.
- The existing tests called “Unreadable” use nonexistent paths rather than manipulating permissions; the revised tests can keep portable failure coverage by treating nonexistent and directory inputs separately.
- Current CodeQL guidance for `cpp/path-injection` recommends normalization plus relative-path/`..` rejection or containment within a safe directory for untrusted path input. Nestor has no such directory contract and the issue explicitly characterizes these variables as operator-controlled, so canonicalization alone may not sanitize the static taint flow.
- CodeQL supports `// codeql[cpp/path-injection]` suppression immediately before the alerting sink. Consolidating validation into one helper permits one documented suppression instead of two raw environment-to-`ifstream` flows.
- The runtime policy will accept operator-selected relative, absolute, and symlink paths only after resolving them to their canonical target; it will reject nonexistent targets and directories before `ifstream`, then store only the validated canonical strings.
- The focused verification command is structurally supported by `scripts/test.sh:3-5` and CTest’s discovered `TlsConfigTest.*` names, but it could not be baseline-executed in this planning environment because `cmake` is absent.
