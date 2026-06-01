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

if(${PROJECT_NAME}_ENABLE_SANITIZERS)
  message(STATUS "Sanitizers enabled: AddressSanitizer + UndefinedBehaviorSanitizer")
  add_compile_options(
    -fsanitize=address
    -fsanitize=undefined
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all
    -g)
  add_link_options(
    -fsanitize=address
    -fsanitize=undefined)
endif()
