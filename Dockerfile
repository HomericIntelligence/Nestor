FROM ubuntu@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b AS builder
# ubuntu:24.04 — pinned for reproducible builds (#60)

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    make \
    g++ \
    git \
    ca-certificates \
    libssl-dev \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Install Conan inside an isolated venv to avoid PEP 668 / system-package
# conflicts (no --break-system-packages). Symlink the entrypoint onto PATH.
RUN python3 -m venv /opt/conan-venv \
    && /opt/conan-venv/bin/pip install --no-cache-dir conan \
    && ln -s /opt/conan-venv/bin/conan /usr/local/bin/conan \
    && conan profile detect --force

WORKDIR /src

# Copy Conan files first for dependency caching.
COPY conanfile.py ./
COPY conan/ conan/
RUN conan install . \
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

RUN cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DProjectNestor_BUILD_TESTING=OFF \
    -DProjectNestor_ENABLE_CLANG_TIDY=OFF \
    -DProjectNestor_ENABLE_CPPCHECK=OFF \
    && cmake --build build --target ProjectNestor_server

# ── Runtime image ─────────────────────────────────────────────────────────────
FROM ubuntu@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b
# ubuntu:24.04 — pinned for reproducible builds (#60)

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    wget \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ProjectNestor_server /usr/local/bin/ProjectNestor_server

EXPOSE 8081

ENV NESTOR_PORT=8081
ENV NATS_URL=nats://localhost:4222

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://localhost:${NESTOR_PORT}/v1/health || exit 1

RUN useradd -r -s /usr/sbin/nologin nestor
USER nestor

CMD ["ProjectNestor_server"]
