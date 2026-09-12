# Local CI execution contract

`just ci-build` builds `nestor-ci:local` for the native Linux architecture.
`just ci-all` runs the local build, test and tool checks with that image.
Podman is preferred; `CONTAINER_ENGINE` selects an explicitly installed engine.
No VM or emulation configuration is changed by these commands.

## Native tool selection

The pinned Ubuntu 24.04 index contains amd64 and arm64 images. The image uses
`ci/install-tool.sh` to select the matching release from `dpkg --print-architecture`.
An unknown architecture fails before a download. Each archive must match its
pinned SHA-256 before the installer extracts the selected executables.

| Tool | Version | Release source |
| --- | --- | --- |
| uv | 0.12.1 | [Astral release](https://github.com/astral-sh/uv/releases/tag/0.12.1) |
| actionlint | 1.7.7 | [actionlint release](https://github.com/rhysd/actionlint/releases/tag/v1.7.7) |
| Gitleaks | 8.30.1 | [Gitleaks release](https://github.com/gitleaks/gitleaks/releases/tag/v8.30.1) |
| Trivy | 0.69.3 | [Trivy release](https://github.com/aquasecurity/trivy/releases/tag/v0.69.3) |

The amd64 versions and checksums are retained. The arm64 hashes come from the
same releases' published checksum files. The C++ tools still come from the
existing `uv.lock`; native selection does not relax `uv sync --locked`.

## Result ownership

Every step executes with fail-fast shell behavior in its own subshell. The
outer runner collects failed step names and returns nonzero if any step fails.
A failed configure or build cannot continue into that step's test invocation.
An existing CMake cache is an incremental input; each build request still
configures and builds the current source. Unit, integration and concurrency
selections fail when no tests match. A failed labelled selection is not
replaced by a different selection. Build parallelism defaults to two jobs.

| Local subset | Executed checks |
| --- | --- |
| `lint` | Controlled CI contracts, clang-format, YAML lint, debug build with clang-tidy |
| `build` | Release dependency preparation, configure and build |
| `unit`, `integration` | Current release build and the required CTest label |
| `concurrency` | Current release build and five runs of the concurrency label |
| `nats` | Current release build and the live-NATS label with an owned broker |
| `actionlint` | Workflow syntax/tool validation |
| `security` | Trivy and the explicitly configured Conan audit |
| `secrets` | Gitleaks |
| `schema` | Workflow schemas and merge-queue policy tests |
| `deps-version-sync`, `uv-check` | CMake version parsing and lockfile consistency |

The live-broker subset runs on the Linux engine host. It requests private
loopback ports from the engine and passes the observed endpoint to CTest.
Private container-ID files bind cleanup to this invocation; a client failure
also requests removal of its created test container. Failed removal returns
a nonzero result and retains the private receipts for reconciliation.
Broker bounce commands use only the
recorded broker ID and the explicit local engine socket.

`all` includes every subset in this table. The controlled launcher contracts
also have a native entry point, `just ci-contract-test`. They substitute external
tools and engine responses while executing the actual shell blocks. They are
regression evidence for orchestration, not proof that a compiler, scanner,
container or broker ran successfully.

## Scanner policy and remaining gates

Gitleaks and Trivy findings remain advisory through their existing
`--exit-code 0` configuration. Trivy retains `HIGH,CRITICAL` selection.
An actual executable or transport failure still fails its CI step. Conan audit
runs only when `CONAN_AUDIT_PROVIDER_TOKEN` is configured; otherwise the runner
reports the omitted audit. A successful advisory scan does not mean that it
found no vulnerabilities or secrets. The historical workflow comment describing
strict Gitleaks enforcement does not match the executed scanner policy.

The local aggregate is not a receipt for every hosted workflow. Hosted compiler
and sanitizer matrices, coverage, Markdown and repository policy checks,
container packaging/smoke checks, and forge-dependent protection checks retain
their separate required results. Successful local checks do not authorize a
merge without current-source hosted CI and the repository's review gates.

The first measured amd64 image build on the laptop failed with QEMU signal 11
at `uv sync --locked`, before tests ran. The native-architecture installer
addresses that execution path; an actual corrected image build and complete
local suite must still be measured. Fleet intake's controlled GitHub tests do
not prove live GitHub writes, research admission, or multi-host performance.
