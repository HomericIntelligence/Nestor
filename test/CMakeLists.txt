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
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "-*Routes*-SanitizerProof.*"
  PROPERTIES LABELS "unit"
)
gtest_discover_tests(${PROJECT_NAME}_tests
  TEST_FILTER "*Routes*"
  TEST_PREFIX "integration."
  PROPERTIES LABELS "integration"
)

if(${PROJECT_NAME}_ENABLE_SANITIZERS)
  if("address" IN_LIST ${PROJECT_NAME}_SANITIZER)
    add_test(NAME ProjectNestor.SanitizerProof.ASAN
      COMMAND $<TARGET_FILE:${PROJECT_NAME}_tests>
              --gtest_filter=SanitizerProof.HeapUseAfterFree)
    set_tests_properties(ProjectNestor.SanitizerProof.ASAN PROPERTIES
      WILL_FAIL ON LABELS "sanitizer")
  endif()

  if("thread" IN_LIST ${PROJECT_NAME}_SANITIZER)
    add_test(NAME ProjectNestor.SanitizerProof.TSAN
      COMMAND $<TARGET_FILE:${PROJECT_NAME}_tests>
              --gtest_filter=SanitizerProof.DataRace)
    set_tests_properties(ProjectNestor.SanitizerProof.TSAN PROPERTIES
      WILL_FAIL ON LABELS "sanitizer")
  endif()
endif()
