# Conan Profiles

Nestor ships two project-level Conan profiles that compose with your
auto-detected host profile:

- `nestor-debug` — Debug build, C++20
- `nestor-release` — Release build, C++20

## Why `nestor-*` naming?

Conan 2's `include(name)` resolves the cache profile directory first, then
falls back to the including file's directory. Naming project profiles `default`
creates a self-inclusion loop on machines with no cache `default` profile. The
`nestor-` prefix eliminates this footgun structurally.

## One-time bootstrap

The project profiles use `include(default)` to inherit your host's
auto-detected settings (OS, arch, compiler, compiler version, libcxx). On a
fresh machine, generate the cache `default` profile once:

```bash
conan profile detect --exist-ok
```

`just deps` and `just deps-release` run this automatically via the
`_conan-bootstrap` recipe. `--exist-ok` is a no-op if `~/.conan2/profiles/default`
already exists, preserving any hand-tuned settings.

> **Note:** `--exist-ok` requires Conan >= 2.3 (the minimum pinned in
> `pyproject.toml`). If you installed Conan independently and are on an older
> version, upgrade or run `conan profile detect --force` manually once.

## Cross-compilation

To build for a different host while keeping native build tools:

```bash
conan install . \
  --profile:host=conan/profiles/nestor-release \
  --profile:build=conan/profiles/nestor-release \
  --output-folder=build/release \
  --build=missing
```

For a fully custom host profile, create one that inherits your target
platform's base profile and pass it as `--profile:host=`.

## ARM64 / macOS contributors

uv installs the CMake/Ninja/Conan wheels cross-platform, but if a wheel is
unavailable for your platform (macOS/ARM64 support varies by release), install
Conan, CMake, and Ninja directly, then:

```bash
conan profile detect --exist-ok   # generates ~/.conan2/profiles/default
conan install . --profile:all=conan/profiles/nestor-debug --output-folder=build/debug --build=missing
cmake --preset debug && cmake --build --preset debug
```

The project profiles contain no host-specific settings, so they compose
correctly with whatever `conan profile detect` produces on your platform.
