# ── uv binary source ──────────────────────────────────────────────────────────
# Pulled as a named stage so `COPY --from=uv` resolves identically under both
# podman/buildah and docker. A bare `COPY --from=ghcr.io/astral-sh/uv:<tag>@<digest>`
# (tag AND digest together) is rejected by buildah with "no stage or image found
# with that name", so we alias the digest-pinned image to a stage name here and
# COPY from the alias. Keep this pin in sync with astral-sh/setup-uv in
# .github/workflows/*.yml when bumping.
FROM ghcr.io/astral-sh/uv:0.12.2@sha256:069a51314a7bb6031777a9273205fe1b0b19e914ef418207d1338b268df641dd AS uv

# ── Builder ───────────────────────────────────────────────────────────────────
FROM ubuntu@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03 AS builder
# ubuntu:24.04 — pinned for reproducible builds (#60)

# apt provides only the compiler + OpenSSL headers + git/ca-certificates. The
# CMake/Ninja/Conan build toolchain is managed by uv as locked PyPI wheels
# (Odysseus ADR-018), not apt. apt packages unpinned on purpose: the base image
# digest above is the version lock (same policy as AchaeanFleet's fleet-wide
# hadolint config).
# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    g++ \
    git \
    ca-certificates \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# uv binary (manages cmake/ninja/conan/gcovr/pre-commit as locked wheels).
COPY --from=uv /uv /uvx /usr/local/bin/

WORKDIR /src

# Sync the locked build toolchain first so it caches independently of sources.
COPY pyproject.toml uv.lock ./
RUN uv sync --locked

# conan's `include(default)` profile autodetects the system g++ installed above.
RUN uv run conan profile detect --force

# Copy Conan files first for dependency caching.
COPY conanfile.py ./
COPY conan/ conan/
RUN uv run conan install . \
    --output-folder=build \
    --profile:all=conan/profiles/nestor-release \
    --build=missing

# Copy CMake configuration so FetchContent (nats.c) can be cached separately.
COPY CMakeLists.txt ./
COPY cmake/ cmake/

# Copy source tree.
COPY include/ include/
COPY src/ src/
COPY test/ test/

RUN uv run cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DNestor_BUILD_TESTING=OFF \
    -DNestor_ENABLE_CLANG_TIDY=OFF \
    -DNestor_ENABLE_CPPCHECK=OFF \
    && uv run cmake --build build --target Nestor_server

# ── Runtime image ─────────────────────────────────────────────────────────────
FROM ubuntu@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03
# ubuntu:24.04 — pinned for reproducible builds (#60)

# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    wget \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/Nestor_server /usr/local/bin/Nestor_server

EXPOSE 8081

ENV NESTOR_PORT=8081
ENV NATS_URL=nats://localhost:4222

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://localhost:${NESTOR_PORT}/v1/health || exit 1

RUN useradd -r -s /usr/sbin/nologin nestor
USER nestor

CMD ["Nestor_server"]
