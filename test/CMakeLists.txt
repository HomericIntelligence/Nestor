cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

add_executable(${PROJECT_NAME}_tests
  src/test_main.cpp
  src/test_store.cpp
  src/test_store_concurrent.cpp
  src/test_nats_client.cpp
  src/test_routes.cpp
  src/test_auth.cpp
  src/test_trace_context.cpp
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

include(GoogleTest)
# Label tests so the unit/integration/concurrency split in CI works:
#   - Tests in the "Routes" suite exercise the HTTP layer end-to-end and are
#     labelled "integration".
#   - Tests in the "StoreConcurrencyTest" suite are labelled "concurrency" and
#     excluded from the "unit" pass so they do not inflate fast-feedback times.
#   - All other tests are labelled "unit".
# Three discovery passes are required because gtest_discover_tests applies one
# LABELS property per call.  The GTest filter "*-A:B" uses the
# positive*-negative syntax to exclude both Routes and StoreConcurrencyTest
# from the unit pass (GTest colon separates negative patterns after the "-").
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "*-*Routes*:*StoreConcurrency*"
  PROPERTIES LABELS "unit"
)
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "*Routes*"
  TEST_PREFIX "integration."
  PROPERTIES LABELS "integration"
)
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "*StoreConcurrency*"
  TEST_PREFIX "concurrency."
  PROPERTIES LABELS "concurrency"
)
