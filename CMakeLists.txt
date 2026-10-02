cmake_minimum_required(VERSION 3.24)
project(postflop-solver-cpp LANGUAGES CXX VERSION 0.1.0)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(PfsFloatingPoint)

if(CMAKE_SIZEOF_VOID_P LESS 8)
  message(FATAL_ERROR
    "A 64-bit target is required: on 32-bit x86 the x87 stack gives excess "
    "precision to float multiplication, which breaks inner_product's "
    "`(x * y) as f64` accumulation.")
endif()

pfs_guard_floating_point()

option(PFS_ENABLE_THREADS       "Multithreaded solve (Rust: rayon)"           ON)
option(PFS_ENABLE_SERIALIZATION "Save/load support (Rust: bincode)"           ON)
option(PFS_ENABLE_ZSTD          "zstd compression for save/load"              OFF)
option(PFS_CUSTOM_ALLOC         "Thread-local bump arena for solver temps"    OFF)
option(PFS_BUILD_TESTS          "Build the test suite"       ${PROJECT_IS_TOP_LEVEL})
option(PFS_BUILD_EXAMPLES       "Build the examples"         ${PROJECT_IS_TOP_LEVEL})
option(PFS_BUILD_SOLVER         "Build the postflop-solver application" ${PROJECT_IS_TOP_LEVEL})
set(PFS_ARCH "" CACHE STRING "ISA baseline: '' | AVX2")

set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

configure_file(cmake/config.hpp.in
  ${PROJECT_BINARY_DIR}/generated/include/pfs/config.hpp @ONLY)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(pfs
  src/core/panic.cpp
  src/core/arena.cpp
  src/core/thread_pool.cpp
  src/card.cpp
  src/hand_table.cpp
  src/range.cpp
  src/bet_size.cpp
  src/action_tree.cpp
  src/card_config.cpp
  src/bunching.cpp
  src/game/base.cpp
  src/game/evaluation.cpp
  src/game/interpreter.cpp
  src/game/bunching_setup.cpp
  $<$<BOOL:${PFS_ENABLE_SERIALIZATION}>:src/game/serialization.cpp>
  $<$<BOOL:${PFS_ENABLE_SERIALIZATION}>:src/file.cpp>
)
add_library(pfs::pfs ALIAS pfs)

target_compile_features(pfs PUBLIC cxx_std_20)
target_include_directories(pfs
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

# PUBLIC: solve<G>() and the slice kernels are templates/inline, so they are
# compiled in the consumer's translation unit and need the same FP semantics.
pfs_apply_floating_point(pfs)

if(MSVC)
  # C4324: expected -- Job/Worker are deliberately alignas(64) to avoid false sharing.
  target_compile_options(pfs PRIVATE /W4 /permissive- /utf-8 /bigobj /wd4324)
  target_compile_options(pfs PUBLIC /Zc:preprocessor /Zc:__cplusplus /EHsc)
  if(PFS_ARCH STREQUAL "AVX2")
    target_compile_options(pfs PUBLIC /arch:AVX2)
  endif()
else()
  target_compile_options(pfs PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wdouble-promotion -Wfloat-conversion)
  target_compile_options(pfs PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
  if(PFS_ARCH STREQUAL "AVX2")
    target_compile_options(pfs PUBLIC -march=x86-64-v3)
  endif()
endif()

if(PFS_ENABLE_THREADS)
  find_package(Threads REQUIRED)
  target_link_libraries(pfs PUBLIC Threads::Threads)
endif()

if(PFS_ENABLE_ZSTD)
  find_package(zstd CONFIG QUIET)
  if(TARGET zstd::libzstd_static)
    target_link_libraries(pfs PRIVATE zstd::libzstd_static)
  elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(pfs PRIVATE zstd::libzstd_shared)
  else()
    message(WARNING "zstd not found; disabling PFS_ENABLE_ZSTD")
    set(PFS_ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
    configure_file(cmake/config.hpp.in
      ${PROJECT_BINARY_DIR}/generated/include/pfs/config.hpp @ONLY)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests, examples and the solver application
# ---------------------------------------------------------------------------
if(PFS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PFS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(PFS_BUILD_SOLVER)
  add_subdirectory(solver)
endif()
