cmake_minimum_required(VERSION 3.16)
project(trace_bench LANGUAGES CXX)

# ---- defaults --------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(WITH_PERFETTO "Build the Perfetto-instrumented variant" ON)
option(WITH_TRACY    "Build the Tracy-instrumented variant"    ON)
# By DEFAULT Tracy is built exactly as the aqua DU production target does:
# on-demand + every background feature disabled + low-res timer fallback (the
# "lowest overhead" config whose cost we actually care about). Set
# TRACY_LIVE=ON to instead build a full default Tracy (on-demand off, all
# features on) -- the heavier path that always exercises the producer hot loop,
# i.e. the "fair comparison" point against Perfetto's always-on session.
option(TRACY_LIVE "Build full live Tracy instead of the default aqua-DU config" OFF)
# Path to a wolfpld/tracy checkout (must contain public/TracyClient.cpp).
# Defaults to the bundled submodule at third_party/tracy; override with
# -DTRACY_SOURCE_DIR=/some/other/tracy to use an external checkout.
set(TRACY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy"
    CACHE PATH "Path to a Tracy (wolfpld) checkout")

set(BENCH_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/trace_backend.cpp)
set(BENCH_INC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(BENCH_WARN -Wall -Wextra)

# ---- baseline: no instrumentation compiled in ------------------------------
add_executable(bench_none ${BENCH_SRC})
target_include_directories(bench_none PRIVATE ${BENCH_INC})
target_compile_definitions(bench_none PRIVATE BENCH_BACKEND=0)
target_compile_options(bench_none PRIVATE ${BENCH_WARN})
target_link_libraries(bench_none PRIVATE Threads::Threads)

# ---- Perfetto variant ------------------------------------------------------
if(WITH_PERFETTO)
  set(PERFETTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/perfetto)
  if(EXISTS ${PERFETTO_DIR}/perfetto.cc)
    # Vendored amalgamated SDK -- built as its own lib (no -Werror, it's 3rd party).
    add_library(perfetto_sdk STATIC ${PERFETTO_DIR}/perfetto.cc)
    target_include_directories(perfetto_sdk PUBLIC ${PERFETTO_DIR})
    target_compile_features(perfetto_sdk PUBLIC cxx_std_17)
    target_link_libraries(perfetto_sdk PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

    add_executable(bench_perfetto ${BENCH_SRC})
    target_include_directories(bench_perfetto PRIVATE ${BENCH_INC})
    target_compile_definitions(bench_perfetto PRIVATE BENCH_BACKEND=1)
    target_compile_options(bench_perfetto PRIVATE ${BENCH_WARN})
    target_link_libraries(bench_perfetto PRIVATE perfetto_sdk Threads::Threads)
  else()
    message(WARNING "third_party/perfetto/perfetto.cc not found; skipping bench_perfetto")
  endif()
endif()

# ---- Tracy variant ---------------------------------------------------------
if(WITH_TRACY AND TRACY_SOURCE_DIR)
  if(EXISTS ${TRACY_SOURCE_DIR}/public/TracyClient.cpp)
    # Turn the client on (default is OFF).
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_STATIC ON CACHE BOOL "" FORCE)
    if(TRACY_LIVE)
      # Full default Tracy: on-demand off, all features on, so the producer hot
      # path is exercised from the first zone whether or not a client connects.
      set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
      message(STATUS "trace-bench: Tracy built in full live mode (TRACY_LIVE=ON)")
    else()
      # DEFAULT: exact replica of the aqua DU production Tracy config
      # ("lowest overhead"). Keep this block in sync with the DU's
      # External/tracy setup.
      set(TRACY_ON_DEMAND            ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_CALLSTACK         OFF CACHE BOOL "" FORCE)
      set(TRACY_NO_CALLSTACK_INLINES OFF CACHE BOOL "" FORCE)
      set(TRACY_NO_CONTEXT_SWITCH    ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_EXIT              ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_SAMPLING          ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_VSYNC_CAPTURE     ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_FRAME_IMAGE       ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_SYSTEM_TRACING    ON  CACHE BOOL "" FORCE)
      set(TRACY_NO_CRASH_HANDLER     ON  CACHE BOOL "" FORCE)
      set(TRACY_TIMER_FALLBACK       ON  CACHE BOOL "" FORCE)
      message(STATUS "trace-bench: Tracy built with aqua DU production profile (default)")
    endif()
    add_subdirectory(${TRACY_SOURCE_DIR} ${CMAKE_BINARY_DIR}/tracy-build)

    add_executable(bench_tracy ${BENCH_SRC})
    target_include_directories(bench_tracy PRIVATE ${BENCH_INC})
    target_compile_definitions(bench_tracy PRIVATE BENCH_BACKEND=2)
    target_compile_options(bench_tracy PRIVATE ${BENCH_WARN})
    target_link_libraries(bench_tracy PRIVATE Tracy::TracyClient Threads::Threads)
  else()
    message(WARNING "TRACY_SOURCE_DIR='${TRACY_SOURCE_DIR}' has no public/TracyClient.cpp; skipping bench_tracy")
  endif()
elseif(WITH_TRACY)
  message(STATUS "TRACY_SOURCE_DIR not set; skipping bench_tracy "
                 "(configure with -DTRACY_SOURCE_DIR=/path/to/tracy)")
endif()
