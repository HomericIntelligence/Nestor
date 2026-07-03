cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

add_executable(${PROJECT_NAME}_tests
  src/test_main.cpp
  src/test_store.cpp
  src/test_store_concurrent.cpp
  src/test_nats_client.cpp
  src/test_nats_client_live.cpp
  src/test_reconnect_policy.cpp
  src/test_routes.cpp
  src/test_auth.cpp
  src/test_trace_context.cpp
  src/test_rate_limiter.cpp
  src/test_tls_config.cpp
  src/test_security_warnings.cpp
  src/test_server_tls.cpp
  src/test_sanitizers.cpp
)

target_compile_features(${PROJECT_NAME}_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(${PROJECT_NAME}_tests PRIVATE
  ${gtest_INCLUDE_DIRS_RELEASE}
)

# Expose the repository source directory so tests that statically inspect
# source files (e.g. the auth-token-logging guard) can locate them
# regardless of where the build or checkout lives.
target_compile_definitions(${PROJECT_NAME}_tests
  PRIVATE PROJECTNESTOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

target_link_libraries(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    ${PROJECT_NAME}::core
    GTest::gtest_main)

enable_sanitizers(${PROJECT_NAME}_tests)

include(GoogleTest)
# Label tests so the unit/integration/concurrency split in CI works:
#   - Tests in the "Routes" suite exercise the HTTP layer end-to-end and are
#     labelled "integration".
#   - Tests in the "StoreConcurrencyTest" suite are labelled "concurrency" and
#     excluded from the "unit" pass so they do not inflate fast-feedback times.
#   - All other tests are labelled "unit".
#   - SanitizerProof tests are registered manually with WILL_FAIL properties.
# Three discovery passes are required because gtest_discover_tests applies one
# LABELS property per call.  The GTest filter "*-A:B" uses the
# positive*-negative syntax to exclude Routes, StoreConcurrencyTest, and the
# manually-registered SanitizerProof tests from the unit pass (GTest colon
# separates negative patterns after the "-").  SanitizerProof tests are
# registered separately via add_test() below with WILL_FAIL properties, so they
# must not be discovered here.
# DISCOVERY_MODE PRE_TEST defers discovery to test-execution time so it works
# under sanitizer builds (ASan runtime is not linked at CMake-configure time).
#
# ThreadSanitizer cannot run at CMake time for PRE_TEST discovery (memory
# mapping failures), so for TSan builds we skip discovery entirely and rely only
# on the manually-registered SanitizerProof.TSAN test below.
if("thread" IN_LIST ${PROJECT_NAME}_SANITIZER)
  message(STATUS "ThreadSanitizer detected: skipping auto-discovery, registering sanitizer tests only")
else()
  gtest_discover_tests(${PROJECT_NAME}_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "*-*Routes*:*StoreConcurrency*:SanitizerProof.*:*NatsClientLive*"
    PROPERTIES LABELS "unit"
  )
  gtest_discover_tests(${PROJECT_NAME}_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "*Routes*"
    TEST_PREFIX "integration."
    PROPERTIES LABELS "integration"
  )
  gtest_discover_tests(${PROJECT_NAME}_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "*StoreConcurrency*"
    TEST_PREFIX "concurrency."
    PROPERTIES LABELS "concurrency"
  )
  # Live-broker NATS tests (issue #119): run by the nats-integration-tests CI
  # job, or locally with test/docker/docker-compose.nats.yml up. The tests
  # self-skip when NESTOR_LIVE_NATS_URL is unset, so including them in the
  # default ctest presets is safe. RUN_SERIAL keeps the broker-bounce tests
  # from interleaving with other live tests under `ctest -j N` — they all
  # share one broker.
  gtest_discover_tests(${PROJECT_NAME}_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "*NatsClientLive*"
    TEST_PREFIX "live."
    PROPERTIES LABELS "live-nats" TIMEOUT 180 RUN_SERIAL TRUE
  )
endif()

if(${PROJECT_NAME}_ENABLE_SANITIZERS)
  if("address" IN_LIST ${PROJECT_NAME}_SANITIZER)
    add_test(NAME ProjectNestor.SanitizerProof.ASAN
      COMMAND $<TARGET_FILE:${PROJECT_NAME}_tests>
              --gtest_filter=SanitizerProof.HeapUseAfterFree)
    # WILL_FAIL inverts a clean non-zero exit, but it treats a crash/signal
    # (SIGABRT) as a hard failure rather than the expected outcome. The asan
    # test preset sets abort_on_error=1, which makes the detected
    # use-after-free abort via SIGABRT. Override it to 0 for this proof test so
    # the sanitizer reports the error and exit()s with a non-zero code that
    # WILL_FAIL can invert.
    set_tests_properties(ProjectNestor.SanitizerProof.ASAN PROPERTIES
      WILL_FAIL ON LABELS "sanitizer"
      ENVIRONMENT "ASAN_OPTIONS=abort_on_error=0:halt_on_error=1")
  endif()

  if("thread" IN_LIST ${PROJECT_NAME}_SANITIZER)
    add_test(NAME ProjectNestor.SanitizerProof.TSAN
      COMMAND $<TARGET_FILE:${PROJECT_NAME}_tests>
              --gtest_filter=SanitizerProof.DataRace)
    set_tests_properties(ProjectNestor.SanitizerProof.TSAN PROPERTIES
      WILL_FAIL ON LABELS "sanitizer"
      ENVIRONMENT "TSAN_OPTIONS=abort_on_error=0:halt_on_error=1")
  endif()
endif()
