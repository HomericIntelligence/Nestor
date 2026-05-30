option(${PROJECT_NAME}_BUILD_TESTING "Build tests" ON)
option(${PROJECT_NAME}_ENABLE_DOXYGEN "Enable Doxygen documentation" OFF)
option(${PROJECT_NAME}_ENABLE_SANITIZERS "Enable sanitizers" OFF)
option(${PROJECT_NAME}_ENABLE_COVERAGE "Enable coverage reporting" OFF)
option(${PROJECT_NAME}_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

if(${PROJECT_NAME}_ENABLE_COVERAGE)
  message(STATUS "Coverage enabled")
  add_compile_options(-O0 --coverage)
  add_link_options(--coverage)
endif()

# Issue #22/#43: Wire the ENABLE_SANITIZERS option to actual compiler flags.
# Previously the option was declared but never applied, making ASAN/UBSAN a no-op.
if(${PROJECT_NAME}_ENABLE_SANITIZERS)
  message(STATUS "Sanitizers enabled: address,undefined")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
