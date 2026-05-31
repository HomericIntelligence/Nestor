cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

add_executable(${PROJECT_NAME}_tests
  src/test_main.cpp
  src/test_store.cpp
  src/test_nats_client.cpp
  src/test_routes.cpp
  src/test_sanitizers.cpp
)

target_compile_features(${PROJECT_NAME}_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(${PROJECT_NAME}_tests PRIVATE
  ${gtest_INCLUDE_DIRS_RELEASE}
)

target_link_libraries(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    ${PROJECT_NAME}::core
    GTest::gtest_main)

enable_sanitizers(${PROJECT_NAME}_tests)

include(GoogleTest)
# Label tests so the unit/integration split in CI works (#30):
#   - Tests in the "Routes" suite exercise the HTTP layer end-to-end and are
#     labelled "integration".
#   - All other tests are labelled "unit".
#   - SanitizerProof tests are registered manually with WILL_FAIL properties.
# Note: Discovered tests do not match the gtest filter "-SanitizerProof.*",
# so we register SanitizerProof tests only via add_test() below with WILL_FAIL.
# ThreadSanitizer cannot run at CMake time for test discovery (memory mapping
# issues), so we disable discovery and only register sanitizer tests.
if("thread" IN_LIST ${PROJECT_NAME}_SANITIZER)
  # For TSan builds, skip test discovery since it causes memory mapping failures
  message(STATUS "ThreadSanitizer detected: skipping auto-discovery, registering sanitizer tests only")
else()
  gtest_discover_tests(${PROJECT_NAME}_tests
    TEST_FILTER "-*Routes*:*SanitizerProof*"
    PROPERTIES LABELS "unit"
  )
  gtest_discover_tests(${PROJECT_NAME}_tests
    TEST_FILTER "*Routes*"
    TEST_PREFIX "integration."
    PROPERTIES LABELS "integration"
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
