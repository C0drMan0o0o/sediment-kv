cmake_minimum_required(VERSION 4.2)
project(sediment LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitiser selection: "", "address", "undefined", "thread", or a combination
# such as "address,undefined". Thread cannot be combined with address.
set(SEDIMENT_SANITIZE "" CACHE STRING "Sanitisers to enable (address, undefined, thread)")
set_property(CACHE SEDIMENT_SANITIZE PROPERTY STRINGS "" address undefined "address,undefined" thread)

if(SEDIMENT_SANITIZE)
  string(REPLACE "," ";" _sanitizers "${SEDIMENT_SANITIZE}")
  foreach(_s IN LISTS _sanitizers)
    if(NOT _s MATCHES "^(address|undefined|thread)$")
      message(FATAL_ERROR "SEDIMENT_SANITIZE: unknown sanitiser '${_s}'")
    endif()
  endforeach()
  if("thread" IN_LIST _sanitizers AND "address" IN_LIST _sanitizers)
    message(FATAL_ERROR "SEDIMENT_SANITIZE: 'thread' cannot be combined with 'address'")
  endif()
  # Global so dependencies (GoogleTest) are instrumented consistently.
  add_compile_options(-fsanitize=${SEDIMENT_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SEDIMENT_SANITIZE})
endif()

# Strict warnings apply only to our targets, not to third-party code.
add_library(sediment_warnings INTERFACE)
target_compile_options(sediment_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

add_library(sediment STATIC src/sediment.cpp src/status.cpp src/coding.cpp src/arena.cpp src/internal_key.cpp src/memtable.cpp src/crc32c.cpp src/file.cpp src/wal.cpp src/recovery.cpp)
target_include_directories(sediment PUBLIC include)
target_link_libraries(sediment PRIVATE sediment_warnings)

add_executable(crash_demo tools/crash_demo.cpp)
target_link_libraries(crash_demo PRIVATE sediment sediment_warnings)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  include(GoogleTest)
  add_subdirectory(tests)
endif()
