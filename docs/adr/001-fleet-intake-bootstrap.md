# ADR-001: GitHub metadata bootstrap for Fleet intake

Status: Proposed

## Context

The legacy research API stores pending ideas in memory. It cannot supply a
durable identity for the first GitHub issue. GitHub issue creation has no
idempotency-key or transaction contract that can also create its own durable
intent. A lost issue response must not authorize another creation.

Telemachy's Fleet registration currently requires a reviewed existing epic and
externally enforced writer ownership. Nestor must supply intake identity without
becoming a second owner of Agamemnon's graph, claims, or completion decisions.

## Proposed decision

Use the GitHub Contents API on an explicitly configured private state repository
and existing branch for small Nestor intake metadata records. This is GitHub
storage owned by Nestor for research intake only. It is not a task scheduler or
an alternative work queue.

The fixed namespace is `nestor/intakes/{intakeId}.json`. Store the stable intake
ID, canonical work repository, request/body digests, creation phase, creation
attempt ID, and confirmed issue reference/receipt. Requirements and discussion
belong in the work issue. Do not store raw private interviews, conversation
history, credentials, or model output in these Git records.

A Contents create establishes `prepared`. A replacement carrying the current
blob SHA changes it to `creating`. Only the invocation with a confirmed exact
write acknowledgment and readback may make one issue-create request. Other
writers lose the compare-and-write or observe an existing intent. No timeout,
heartbeat, process restart, or absence from issue search authorizes takeover.

The transport must also fence retries inside its HTTP dependency. For pinned
cpp-httplib 0.18.3, a one-use header writer rejects repeated serialization before
the request buffer is flushed. Reconnection alone does not grant another
transmission. Dependency changes require revalidation of this boundary; TLS
verification, authentication, and bounded I/O remain enabled.

After an uncertain creation, enumerate open and closed issues with bounded
pagination. Adopt exactly one issue with the exact intake marker, title, and
body. Missing, duplicate, edited, or unavailable results require reconciliation.
There is no retry that resets `creating` to `prepared`. A confirmed issue then
permits the `created` receipt write. Repeating a completed request returns the
same issue reference.

The work issue starts as a research intake. It is not automatically a reviewed
epic, delegated task, or approved implementation. Promoting the issue to a
Telemachy workflow requires the existing review and registration contracts.
Agamemnon retains all planning, task admission, dispatch, and completion authority.

## Boundaries and costs

- The explicit Fleet endpoint remains unavailable without complete GitHub state
  configuration. The legacy research API keeps its existing behavior.
- Metadata uses Git history. Removing a current record does not erase earlier
  commits; privacy and retention operations must account for that history.
- An acknowledged intent followed by process death can leave an issue uncreated.
  This safe failure requires operator reconciliation. No exactly-once completion
  or automatic distributed lease is claimed.
- The state branch must be provisioned and protected by the operator. Runtime
  code never creates the repository/branch or force-updates its history.
- Telemachy's current registration writer condition is not removed by this
  bootstrap. This proposal controls one intake issue creation, not every later
  issue-body mutation.

## Validation and acceptance

Controlled tests must demonstrate one winning creation attempt under concurrent
SHA updates, no issue creation after failed persistence, lost issue acknowledgments,
restart reconciliation, exact metadata readback, and privacy field rejection.

The GitHub Contents compare-and-write behavior, permission configuration, and
positive real GitHub admission remain measured deployment gates. Fixture success
does not establish those external properties. This proposal does not claim
research-agent execution, automatic first-epic registration, or Fleet scale results.

## Sources

- [GitHub Contents API](https://docs.github.com/en/rest/repos/contents#create-or-update-file-contents)
  requires the existing blob SHA when replacing a file and documents conflict
  responses.
- [GitHub issue creation API](https://docs.github.com/en/rest/issues/issues#create-an-issue)
  defines the independent issue-create operation.
