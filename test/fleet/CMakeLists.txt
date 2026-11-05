cmake_minimum_required(VERSION 3.20)
project(NestorFleetContracts LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
find_path(JSON_INCLUDE nlohmann/json.hpp REQUIRED)
find_path(HTTPLIB_INCLUDE httplib.h PATH_SUFFIXES include/httplib REQUIRED)
find_path(GTEST_INCLUDE gtest/gtest.h REQUIRED)
find_library(GTEST_LIBRARY gtest REQUIRED)
find_library(GTEST_MAIN_LIBRARY gtest_main REQUIRED)
find_path(NATS_INCLUDE nats.h REQUIRED)
find_library(NATS_LIBRARY NAMES nats_staticd nats_static natsd nats REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(nestor_fleet_core STATIC ${ROOT}/src/store.cpp ${ROOT}/src/auth.cpp
    ${ROOT}/src/trace_context.cpp ${ROOT}/src/rate_limiter.cpp
    ${ROOT}/src/routes.cpp ${ROOT}/src/nats_client.cpp ${ROOT}/src/fleet_intake.cpp
    ${ROOT}/src/fleet_github.cpp ${ROOT}/src/tls_config.cpp ${ROOT}/src/security_warnings.cpp)
target_include_directories(nestor_fleet_core PUBLIC ${ROOT}/include)
target_compile_definitions(nestor_fleet_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(nestor_fleet_core SYSTEM PUBLIC
    ${JSON_INCLUDE} ${HTTPLIB_INCLUDE} ${NATS_INCLUDE})
target_link_libraries(nestor_fleet_core PUBLIC
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${NATS_LIBRARY})
add_executable(nestor_fleet_tests ${ROOT}/test/src/test_store.cpp
    ${ROOT}/test/src/test_store_concurrent.cpp ${ROOT}/test/src/test_auth.cpp
    ${ROOT}/test/src/test_trace_context.cpp ${ROOT}/test/src/test_rate_limiter.cpp
    ${ROOT}/test/src/test_routes.cpp ${ROOT}/test/src/test_fleet_intake.cpp
    ${ROOT}/test/src/test_fleet_github.cpp ${ROOT}/test/src/test_fleet_startup.cpp)
add_executable(nestor_fleet_server ${ROOT}/src/server_main.cpp)
target_link_libraries(nestor_fleet_server PRIVATE nestor_fleet_core)
add_dependencies(nestor_fleet_tests nestor_fleet_server)
target_compile_definitions(nestor_fleet_tests PRIVATE
    NESTOR_TEST_SERVER="$<TARGET_FILE:nestor_fleet_server>")
target_compile_definitions(nestor_fleet_tests PRIVATE NESTOR_SOURCE_DIR="${ROOT}")
target_include_directories(nestor_fleet_tests SYSTEM PRIVATE ${GTEST_INCLUDE})
target_link_libraries(nestor_fleet_tests PRIVATE nestor_fleet_core
    ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY})
add_test(NAME nestor_fleet_contract COMMAND nestor_fleet_tests)
