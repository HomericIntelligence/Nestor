function(set_project_warnings project_name)
  set(MSVC_WARNINGS /W4 /permissive-)
  set(CLANG_WARNINGS
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
      -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
      -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
  set(GCC_WARNINGS
      ${CLANG_WARNINGS}
      -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches
      -Wlogical-op -Wuseless-cast)

  if(MSVC)
    set(PROJECT_WARNINGS_CXX ${MSVC_WARNINGS})
    # Issue #23: Gate /WX on the WARNINGS_AS_ERRORS option.
    if(${CMAKE_PROJECT_NAME}_WARNINGS_AS_ERRORS)
      list(APPEND PROJECT_WARNINGS_CXX /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
    set(PROJECT_WARNINGS_CXX ${CLANG_WARNINGS})
    # Issue #23: Gate -Werror on the WARNINGS_AS_ERRORS option.
    # Previously -Werror was unconditionally included in CLANG_WARNINGS, making
    # the ProjectNestor_WARNINGS_AS_ERRORS=OFF cmake option a no-op.
    if(${CMAKE_PROJECT_NAME}_WARNINGS_AS_ERRORS)
      list(APPEND PROJECT_WARNINGS_CXX -Werror)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(PROJECT_WARNINGS_CXX ${GCC_WARNINGS})
    if(${CMAKE_PROJECT_NAME}_WARNINGS_AS_ERRORS)
      list(APPEND PROJECT_WARNINGS_CXX -Werror)
    endif()
  endif()

  target_compile_options(${project_name} PRIVATE ${PROJECT_WARNINGS_CXX})
endfunction()
