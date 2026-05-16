cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

add_executable(${PROJECT_NAME}_tests
  src/test_main.cpp
  src/test_store.cpp
  src/test_nats_client.cpp
  src/test_routes.cpp
)

target_compile_features(${PROJECT_NAME}_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    ${PROJECT_NAME}::core
    GTest::gtest_main)

include(GoogleTest)
# Label tests so the unit/integration split in CI works (#30):
#   - Tests in the "Routes" suite exercise the HTTP layer end-to-end and are
#     labelled "integration".
#   - All other tests are labelled "unit".
# Two discovery passes are required because gtest_discover_tests applies one
# LABELS property per call.
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "-*Routes*"
  PROPERTIES LABELS "unit"
)
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "*Routes*"
  TEST_PREFIX "integration."
  PROPERTIES LABELS "integration"
)
