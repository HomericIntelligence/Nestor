# Security Deployment Guide — ProjectNestor

**Issue #42:** ProjectNestor binds on `0.0.0.0` and does not terminate TLS itself.
This document mandates deployment requirements and documents the threat model.

## TLS Requirement

ProjectNestor MUST be deployed behind a TLS-terminating reverse proxy. The server does not
handle TLS directly. Acceptable proxy options include:

- **Caddy** (automatic certificate provisioning): `reverse_proxy localhost:8081`
- **nginx** with Let's Encrypt / certbot
- **Tailscale** (all inter-service traffic is already encrypted via WireGuard)
- **Traefik** with ACME integration

Do not expose ProjectNestor on `0.0.0.0:8081` to a public network without TLS termination.

## Authentication

Set `NESTOR_AUTH_TOKEN` to a cryptographically random string (≥32 bytes):

```bash
export NESTOR_AUTH_TOKEN="$(openssl rand -base64 32)"
```

All requests except `GET /v1/health` require an `Authorization: Bearer <token>` header.
Clients that omit this header or provide a wrong token receive HTTP 401.

If `NESTOR_AUTH_TOKEN` is unset, the server starts in **unauthenticated dev mode** and logs
a warning. This mode is acceptable in isolated development environments; it is NEVER
acceptable in production.

## Rate Limiting

Set `NESTOR_RATE_LIMIT_RPS` to the maximum number of requests per second allowed per
authenticated token (or per remote IP when auth is disabled). Default: `100`.

```bash
export NESTOR_RATE_LIMIT_RPS=50
```

Clients exceeding this limit receive HTTP 429. The rate limiter uses a token-bucket
algorithm: bursts up to `RPS` tokens are permitted before throttling begins.

## Network Binding

ProjectNestor binds on `0.0.0.0` by default (`NESTOR_PORT=8081`). To restrict to a
loopback or Tailscale interface, configure your proxy to forward only from that interface,
or run ProjectNestor in a network namespace that isolates it from public interfaces.

## Threat Model

| Threat | Mitigation |
|---|---|
| Unauthenticated API access | `NESTOR_AUTH_TOKEN` bearer-token check (#40/#65) |
| Plaintext credential interception | TLS-terminating proxy required |
| Memory exhaustion via large request body | 64 KiB body-size cap (#41) |
| DoS via request flooding | Token-bucket rate limiter (#44) |
| Credential timing oracle | Constant-time bearer comparison (#40) |
| Unbounded store growth | Store bounded at `kDefaultMaxItems = 10,000` (#21/#48) |
| NATS publish failures | Graceful degradation — server continues without NATS |

## Audit Status

This deployment guide satisfies audit GO-condition #3 from the 2026-04-28 strict audit.
See issue #12 for the full audit remediation tracking.
