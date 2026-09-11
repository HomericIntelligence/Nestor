# Fleet intake bootstrap

The proposed [Fleet intake bootstrap](adr/001-fleet-intake-bootstrap.md) gives
Nestor a durable identity before it creates the canonical research issue.
Agamemnon still owns task planning, claims, dispatch, and completion. Telemachy
still owns workflow registration. No new queue or memory-only admission is added.

## Configuration and API

Fleet intake is disabled by default. Set all of the following on the backend:

| Setting | Required value |
| --- | --- |
| `NESTOR_FLEET_STATE_REPOSITORY` | Existing private repository, canonical lower-case `owner/repository` |
| `NESTOR_FLEET_STATE_BRANCH` | Existing branch; 1–64 ASCII letters, digits, `_`, `-`, or `.`; no slash or `..` |
| `GITHUB_TOKEN` | Backend credential with state Contents read/write and work issue read/write permission |
| `NESTOR_AUTH_MODE` | `required` |
| `NESTOR_AUTH_TOKEN` | Existing Nestor bearer credential |

Partial configuration, invalid values, missing GitHub credentials, or disabled
HTTP authentication fail startup before NATS attachment. With both Fleet settings
absent, the Fleet routes return `503`; an unrelated `GITHUB_TOKEN` does not enable
them. No repository or branch is created by the service. Use TLS or an authenticated
private reverse proxy as described in the existing deployment documentation.

`POST /v1/research/intakes` accepts `application/json` with exactly five fields:

```json
{
  "schema": "hi/nestor/intake-request/v1",
  "intakeId": "research-20260911-example",
  "workRepository": "homericintelligence/odyssey",
  "title": "Research the proposed experiment",
  "body": "Publishable requirements and context for the research issue."
}
```

The caller retains the same `intakeId` and exact requirements for every retry.
IDs contain 8–64 lower-case ASCII letters, digits, `_`, or `-`, beginning with a
letter or digit. Titles contain 1–256 bytes and bodies at most 60,000 bytes; the
serialized HTTP request is limited to 65,536 bytes. The reserved
`nestor:fleet-intake:` marker cannot appear in the supplied body. GitHub repository
case is normalized before request identity is calculated.

A successful submission returns `200` with the durable metadata record below.
`GET /v1/research/intakes/{intakeId}` returns the current record, including a
`creating` phase that needs reconciliation. Both routes use existing bearer
authentication and rate limiting. Errors expose a stable `error` code, never the
submitted text or backend credential: `400` invalid input, `404` missing intake,
`409` conflict/reconciliation requirement, and `503` unavailable or unconfirmed
persistence. Retry with unchanged input after a `503`; this can reconcile an
existing attempt but cannot authorize another issue creation.

## Storage contract

The configured private GitHub state repository and branch hold
`nestor/intakes/{intakeId}.json` with schema `hi/nestor/intake/v1`:

- `intakeId`: stable caller-supplied identity; retries retain it.
- `workRepository`: canonical lower-case GitHub owner/repository.
- `requestDigest`, `bodyDigest`: SHA-256 values binding publishable requirements.
- `phase`: `prepared`, `creating`, or `created`.
- `generation`: initial creation generation; there is no automatic renewal.
- `attemptId`: unique identity of the recorded creation attempt.
- `issue`, `receipt`: confirmed work issue link and observation receipt.

Only metadata belongs in this record. The publishable title/body are sent to the
work issue. A completed research intake is not an approved epic or completed
implementation. Private interview text and runtime conversation history stay in
their private backend/worker storage.

## Persistence and retry

The creation path confirms a Contents write and exact readback before proceeding.
The `prepared` to `creating` transition includes the prior blob SHA. A compare
conflict or uncertain response cannot authorize an issue POST. The issue-create
transport makes one attempt and never retries internally.

If the issue response is lost, another invocation reads `creating` and searches
open and closed issues for the exact marker/body. Exactly one matching issue can
be recorded. No match, duplicate markers, edited issue content, authentication
failure, or incomplete pagination remains a reconciliation requirement. It does
not grant another attempt. Enumeration stops at 100 pages, 16 MiB of returned
JSON, or its 30-second processing budget; a single HTTP request has its own timeout.

Unknown fields in stored metadata are rejected before the read model is returned.
The state namespace must remain private, and its branch must already exist.
Closing a work issue does not erase the intake or authorize another creation.

## Implementation and evidence boundaries

The implementation includes the intake service, HTTP routes, startup validation,
and GitHub Contents adapter. The default transport uses certificate-verified HTTPS
to `api.github.com`, no redirects or automatic mutation retry, a three-second
connect timeout, five-second read/write timeouts, and an 8 MiB response limit.
Credentials remain on the backend. The injected transport/client seams exist for
controlled tests; the server exposes no alternate GitHub endpoint setting.

Focused native tests exercise concurrent callers, controlled GitHub responses,
actual loopback HTTP transport, and the compiled server's startup rejection. They
also cover lost creation acknowledgments, exact readback, closed issue
reconciliation, and metadata validation. Full intake-to-research-agent execution
and reviewed promotion to Telemachy's existing-epic registration remain separate
steps. This endpoint does not publish a new research event or alter Agamemnon state.

Use `just fleet-configure` with explicit cached dependency paths, then
`just fleet-build` and `just fleet-test`. These recipes use the scripts wrapper,
cap builds at two jobs, and never fetch dependencies. The focused native target
does not replace the standard Conan, sanitizer, or hosted CI gates.

No live GitHub writes, configured state branch, provider work, or 108-agent
acceptance run occurred during these tests. Positive GitHub Contents/CAS admission
must be validated with an explicitly approved fixture namespace before promotion.
