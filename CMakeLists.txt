cmake_minimum_required(VERSION 3.20)

project(
  ProjectNestor
  VERSION 0.1.0
  DESCRIPTION "Research, ideation, and search for HomericIntelligence"
  LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory and run CMake from there."
  )
endif()

include(cmake/StandardSettings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Doxygen.cmake)
include(cmake/SourcesAndHeaders.cmake)

add_library(${PROJECT_NAME} ${headers} ${sources})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

enable_sanitizers(${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)

set_project_warnings(${PROJECT_NAME})

# ─── HTTP Server Dependencies ───────────────────────────────────────────────
# cpp-httplib and nlohmann_json are provided by Conan (run `just deps` first).
# nats.c stays as FetchContent (not reliably available on ConanCenter).
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

include(FetchContent)
FetchContent_Declare(
  natsc
  GIT_REPOSITORY https://github.com/nats-io/nats.c.git
  GIT_TAG        v3.9.1
  GIT_SHALLOW    TRUE
)

set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING_SAVED "${BUILD_TESTING}")
set(BUILD_TESTING        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(natsc)
set(BUILD_TESTING "${BUILD_TESTING_SAVED}" CACHE BOOL "" FORCE)

# The vendored nats.c library trips UndefinedBehaviorSanitizer
# (nonnull-attribute in glib_dispatch_pool.c) on code we do not own. With
# -fno-sanitize-recover=all that aborts the test suite. Exclude the
# third-party target from sanitizer instrumentation so our own code stays
# strictly checked while the dependency is not. -fno-sanitize=all is honored
# by both GCC and Clang (unlike -fsanitize-ignorelist, which is Clang-only).
if(${PROJECT_NAME}_ENABLE_SANITIZERS AND TARGET nats_static)
  target_compile_options(nats_static PRIVATE -fno-sanitize=all)
endif()

# ─── Testable Core Library (everything except main) ────────────────────────
add_library(${PROJECT_NAME}_core STATIC
  src/store.cpp
  src/routes.cpp
  src/nats_client.cpp
)
add_library(${PROJECT_NAME}::core ALIAS ${PROJECT_NAME}_core)

enable_sanitizers(${PROJECT_NAME}_core)

target_include_directories(${PROJECT_NAME}_core PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${nlohmann_json_INCLUDE_DIRS}
  ${httplib_INCLUDE_DIRS}
  $<TARGET_PROPERTY:nats_static,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(${PROJECT_NAME}_core PUBLIC
  httplib::httplib
  nlohmann_json::nlohmann_json
  nats_static
  OpenSSL::SSL
  OpenSSL::Crypto
)

target_compile_features(${PROJECT_NAME}_core PUBLIC cxx_std_20)
set_target_properties(${PROJECT_NAME}_core PROPERTIES CXX_EXTENSIONS OFF)

# ─── Server Executable ─────────────────────────────────────────────────────
add_executable(ProjectNestor_server src/server_main.cpp)

enable_sanitizers(ProjectNestor_server)

target_link_libraries(ProjectNestor_server PRIVATE ${PROJECT_NAME}::core)

target_compile_features(ProjectNestor_server PRIVATE cxx_std_20)

if(${PROJECT_NAME}_BUILD_TESTING)
  enable_testing()
  add_subdirectory(test)
endif()

if(${PROJECT_NAME}_ENABLE_DOXYGEN)
  enable_doxygen()
endif()

include(GNUInstallDirs)
install(
  TARGETS ${PROJECT_NAME}
  EXPORT ${PROJECT_NAME}Targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  EXPORT ${PROJECT_NAME}Targets
  FILE ${PROJECT_NAME}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

install(TARGETS ProjectNestor_server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
