cmake_minimum_required(VERSION 3.16)
project(kvsd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# WHY an opt-in rather than always-on: -Werror in a developer's default build turns an
# unrelated compiler upgrade into a broken checkout. CI turns it on; humans opt in.
option(KVSD_WERROR "Treat warnings as errors" OFF)

add_library(kvsd_warnings INTERFACE)
target_compile_options(kvsd_warnings INTERFACE -Wall -Wextra)
if(KVSD_WERROR)
  target_compile_options(kvsd_warnings INTERFACE -Werror)
endif()

# Everything except main.cpp lives in a library so the test binaries can link the
# real implementation instead of a copy compiled with different flags.
add_library(kvsd_core STATIC
  src/net/placeholder.cpp
)
target_include_directories(kvsd_core PUBLIC src)
target_link_libraries(kvsd_core PUBLIC kvsd_warnings)

add_executable(kvsd src/main.cpp)
target_link_libraries(kvsd PRIVATE kvsd_core)

enable_testing()
