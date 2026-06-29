function(enable_sanitizers target)
  if(NOT ${PROJECT_NAME}_ENABLE_SANITIZERS)
    return()
  endif()

  if(${PROJECT_NAME}_ENABLE_COVERAGE)
    message(FATAL_ERROR "${PROJECT_NAME}_ENABLE_SANITIZERS and ${PROJECT_NAME}_ENABLE_COVERAGE cannot both be ON")
  endif()

  if("address" IN_LIST ${PROJECT_NAME}_SANITIZER AND "thread" IN_LIST ${PROJECT_NAME}_SANITIZER)
    message(FATAL_ERROR "${PROJECT_NAME}_SANITIZER cannot contain both 'address' and 'thread'")
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    message(FATAL_ERROR "Sanitizers require GCC, Clang, or AppleClang; got ${CMAKE_CXX_COMPILER_ID}")
  endif()

  # Build the sanitizer flags
  set(sanitizer_flags "")
  foreach(sanitizer IN LISTS ${PROJECT_NAME}_SANITIZER)
    list(APPEND sanitizer_flags "-fsanitize=${sanitizer}")
  endforeach()
  list(APPEND sanitizer_flags "-fno-omit-frame-pointer")
  list(APPEND sanitizer_flags "-fno-sanitize-recover=all")

  # Address sanitizer specific flags
  if("address" IN_LIST ${PROJECT_NAME}_SANITIZER)
    list(APPEND sanitizer_flags "-fno-optimize-sibling-calls")
  endif()

  # Apply compile flags (C++ only via generator expression)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${sanitizer_flags}>
  )

  # Apply link flags (no language genex — link is per-target, not per-TU)
  target_link_options(${target} PRIVATE ${sanitizer_flags})

  # Set compile definitions for test code to gate on specific sanitizers
  if("address" IN_LIST ${PROJECT_NAME}_SANITIZER)
    target_compile_definitions(${target} PRIVATE PROJECTNESTOR_SANITIZER_ADDRESS=1)
  endif()
  if("thread" IN_LIST ${PROJECT_NAME}_SANITIZER)
    target_compile_definitions(${target} PRIVATE PROJECTNESTOR_SANITIZER_THREAD=1)
  endif()

  message(STATUS "Sanitizers enabled for ${target}: ${${PROJECT_NAME}_SANITIZER}")
endfunction()
