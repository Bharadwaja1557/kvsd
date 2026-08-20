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
# WHY compile-time backend selection instead of a runtime interface: only one of the
# two syscall families exists on any given host, so a virtual Poller would add an
# indirection to buy portability the linker can provide for free.
if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
  set(KVSD_POLLER_SRC src/net/poller_kqueue.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(KVSD_POLLER_SRC src/net/poller_epoll.cpp)
else()
  message(FATAL_ERROR "kvsd supports Linux (epoll) and macOS/BSD (kqueue) only; got ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "kvsd poller backend: ${KVSD_POLLER_SRC}")

add_library(kvsd_core STATIC
  src/net/buffer.cpp
  src/net/socket.cpp
  ${KVSD_POLLER_SRC}
  src/net/event_loop.cpp
  src/net/server.cpp
  src/util/log.cpp
  src/resp/parser.cpp
  src/resp/writer.cpp
  src/db/db.cpp
  src/cmd/table.cpp
  src/cmd/connection.cpp
  src/cmd/string.cpp
  src/cmd/keyspace.cpp
  src/cmd/server.cpp
)
target_include_directories(kvsd_core PUBLIC src)
target_link_libraries(kvsd_core PUBLIC kvsd_warnings)

add_executable(kvsd src/main.cpp)
target_link_libraries(kvsd PRIVATE kvsd_core)

enable_testing()

# Each test binary is a plain executable that returns non-zero on failure, which is
# all CTest needs and keeps the no-third-party-dependency rule intact.
function(kvsd_add_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE kvsd_core)
  target_include_directories(${name} PRIVATE tests)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

kvsd_add_test(test_buffer)
kvsd_add_test(test_poller)
kvsd_add_test(test_event_loop)
kvsd_add_test(test_resp)
kvsd_add_test(test_writer)
kvsd_add_test(test_db)
kvsd_add_test(test_cmd)
