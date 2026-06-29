set shell := ["bash", "-c"]

default:
  @just --list

# Bootstrap: create ~/.conan2/profiles/default if it does not exist (no-op if present)
_conan-bootstrap:
  conan profile detect --exist-ok

# Install Conan dependencies (cpp-httplib, nlohmann_json, gtest) — Debug
deps: _conan-bootstrap
  conan install . --output-folder=build/debug --profile:all=conan/profiles/nestor-debug --build=missing

# Install Conan dependencies for release
deps-release: _conan-bootstrap
  conan install . --output-folder=build/release --profile:all=conan/profiles/nestor-release --build=missing

build: deps
  cmake --preset debug && cmake --build --preset debug

test:
  ctest --preset debug --output-on-failure

lint:
  ./scripts/lint.sh

format:
  ./scripts/format.sh

format-check:
  ./scripts/format.sh --check

coverage: _conan-bootstrap
  conan install . --output-folder=build/coverage --profile:all=conan/profiles/nestor-debug --build=missing && \
  cmake --preset coverage && cmake --build --preset coverage && ./scripts/coverage.sh

clean:
  rm -rf build install

ci:
  cmake --preset ci && cmake --build --preset ci && ctest --preset ci

# Generate Doxygen API documentation under build/docs/
docs: _conan-bootstrap
  conan install . --output-folder=build/docs --profile:all=conan/profiles/nestor-debug --build=missing && \
  cmake --preset docs && cmake --build --preset docs

# Install Conan dependencies for AddressSanitizer
deps-asan:
  conan install . --output-folder=build/asan --profile=conan/profiles/debug --build=missing

# Install Conan dependencies for ThreadSanitizer
deps-tsan:
  conan install . --output-folder=build/tsan --profile=conan/profiles/debug --build=missing

# Build and test under AddressSanitizer + UBSan
asan: deps-asan
  cmake --preset asan && cmake --build --preset asan && ctest --preset asan

# Build and test under ThreadSanitizer
tsan: deps-tsan
  cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
