set shell := ["bash", "-c"]

default:
  @just --list

# Install Conan dependencies (cpp-httplib, nlohmann_json, gtest)
deps:
  conan install . --output-folder=build/debug --profile=conan/profiles/debug --build=missing

# Install Conan dependencies for release
deps-release:
  conan install . --output-folder=build/release --profile=conan/profiles/default --build=missing

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

coverage: deps
  cmake --preset coverage && cmake --build --preset coverage && ./scripts/coverage.sh

clean:
  rm -rf build install

ci:
  cmake --preset ci && cmake --build --preset ci && ctest --preset ci

# Generate Doxygen API documentation under build/docs/
docs: deps
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
