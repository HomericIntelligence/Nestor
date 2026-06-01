# Security Policy

## Reporting Security Vulnerabilities

**Do not open public issues for security vulnerabilities.**

We take security seriously. If you discover a security vulnerability, please report it responsibly.

## How to Report

### Email (Preferred)

Send an email to: **<4211002+mvillmow@users.noreply.github.com>**

Or use the GitHub private vulnerability reporting feature if available.

### What to Include

Please include as much of the following information as possible:

- **Description** - Clear description of the vulnerability
- **Impact** - Potential impact and severity assessment
- **Steps to reproduce** - Detailed steps to reproduce the issue
- **Affected files** - Which source files, endpoints, or configurations are affected
- **Suggested fix** - If you have a suggested fix or mitigation

### Example Report

```text
Subject: [SECURITY] Research endpoint returns unfiltered internal data

Description:
The /v1/research endpoint returns internal metadata including store
keys and NATS subject names without authorization checks, leaking
internal service topology to unauthenticated callers.

Impact:
An attacker could enumerate internal NATS subjects and agent IDs
to map the service topology.

Steps to Reproduce:
1. Start ProjectNestor server
2. curl http://<host>:8081/v1/research
3. Observe internal metadata in the response

Affected Files:
src/routes.cpp (research route handlers)

Suggested Fix:
Add authentication middleware and filter internal fields from responses.
```

## Response Timeline

We aim to respond to security reports within the following timeframes:

| Stage                    | Timeframe              |
|--------------------------|------------------------|
| Initial acknowledgment   | 48 hours               |
| Preliminary assessment   | 1 week                 |
| Fix development          | Varies by severity     |
| Public disclosure        | After fix is released  |

## Severity Assessment

We use the following severity levels:

| Severity     | Description                          | Response           |
|--------------|--------------------------------------|--------------------|
| **Critical** | Remote code execution, data breach   | Immediate priority |
| **High**     | Privilege escalation, data exposure  | High priority      |
| **Medium**   | Limited impact vulnerabilities       | Standard priority  |
| **Low**      | Minor issues, hardening              | Scheduled fix      |

## Responsible Disclosure

We follow responsible disclosure practices:

1. **Report privately** - Do not disclose publicly until a fix is available
2. **Allow reasonable time** - Give us time to investigate and develop a fix
3. **Coordinate disclosure** - We will work with you on disclosure timing
4. **Credit** - We will credit you in the security advisory (if desired)

## What We Will Do

When you report a vulnerability:

1. Acknowledge receipt within 48 hours
2. Investigate and validate the report
3. Develop and test a fix
4. Release the fix
5. Publish a security advisory

## Scope

### In Scope

- C++ source code (research routes, store, NATS client)
- REST API endpoints
- CMake build configuration
- Dockerfile and container configuration

### Out of Scope

- Odysseus meta-repo configurations (report to [Odysseus](https://github.com/HomericIntelligence/Odysseus))
- ProjectAgamemnon API (report to [ProjectAgamemnon](https://github.com/HomericIntelligence/ProjectAgamemnon))
- Other HomericIntelligence submodule repos (report to that repo directly)
- Third-party dependencies (report upstream to cpp-httplib, nlohmann_json, etc.)
- Social engineering attacks
- Physical security

## Authentication

All endpoints under `/v1/*` require bearer-token authentication. No endpoint is exempt from this requirement.

### Configuration

Set two environment variables before starting the server:

- **`NESTOR_AUTH_TOKEN`** — the bearer token (required in `required` mode)
- **`NESTOR_AUTH_MODE`** — authentication mode: `"required"` or `"none"` (case-sensitive, lowercase only; defaults to `"required"`)

### Mode Behavior

| Mode      | Behavior                          |
|-----------|-----------------------------------|
| `required` | All `/v1/*` endpoints require a valid Bearer token. Server fails to start if token is missing or empty. |
| `none`    | All endpoints are unauthenticated. Used only in test/dev harnesses with explicit opt-in. |

### Error Response

Requests without a valid Bearer token return **401 Unauthorized**:

```json
{"detail":"unauthorized"}
```

### Example

```bash
# Start the server with authentication enabled
export NESTOR_AUTH_TOKEN="your-secret-token"
export NESTOR_AUTH_MODE="required"
./projectnestor_server

# Unauthenticated request → 401
curl http://localhost:8081/v1/health
# {"detail":"unauthorized"}

# Authenticated request → 200
curl -H "Authorization: Bearer your-secret-token" http://localhost:8081/v1/health
# {"status":"ok"}
```

### Token Security

- Tokens are compared using constant-time comparison (`CRYPTO_memcmp`) to prevent timing attacks
- Token values are **never logged** in stdout, stderr, or error messages
- Empty strings are treated as unset tokens
- Unknown or mixed-case mode strings cause startup failure (no silent fallback)

## Security Best Practices

When contributing to ProjectNestor:

- Validate all HTTP request input before processing
- Avoid buffer overflows and undefined behavior
- Build with `cmake --preset debug` to run under AddressSanitizer + UndefinedBehaviorSanitizer; investigate any reports
  before merging.
- Never commit secrets, API keys, tokens, or credentials
- Pin FetchContent dependency versions to known-good commits

## Contact

For security-related questions that are not vulnerability reports:

- Open a GitHub Discussion with the "security" tag
- Email: <4211002+mvillmow@users.noreply.github.com>

---

Thank you for helping keep HomericIntelligence secure!
