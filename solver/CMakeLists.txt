# Third party miniz (MIT), the zlib codec of the solution file. Built as C, with its archive,
# stdio and time APIs left out.
enable_language(C)
add_library(pfs_miniz STATIC ${PROJECT_SOURCE_DIR}/third-party/miniz/miniz.c)
target_include_directories(pfs_miniz SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third-party/miniz)
target_compile_definitions(pfs_miniz PUBLIC
  MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(MSVC)
  target_compile_options(pfs_miniz PRIVATE /W0)
else()
  target_compile_options(pfs_miniz PRIVATE -w)
endif()

set(PFS_SOLVER_SOURCES
  src/game_file.hpp
  src/game_file.cpp
  src/gto_range.hpp
  src/gto_range.cpp
  src/result_file.hpp
  src/result_file.cpp
  src/solution_format.hpp
  src/solution_file.hpp
  src/solution_file.cpp
  src/solution_reader.hpp
  src/solution_reader.cpp
)

function(pfs_solver_target target)
  target_link_libraries(${target} PRIVATE pfs::pfs pfs_miniz)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # Third party nlohmann/json (header only), included as <nlohmann/json.hpp>.
  target_include_directories(${target} SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third-party)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 /wd4324)
    # std::span bounds checks in the solver's inline kernels would slow a Release build down.
    target_compile_definitions(${target} PRIVATE
      $<$<CONFIG:Release>:_ITERATOR_DEBUG_LEVEL=0;_CONTAINER_DEBUG_LEVEL=0;_HAS_ITERATOR_DEBUGGING=0>)
    # solve_recursive nests deeply, and a helping worker stacks on top of that.
    target_link_options(${target} PRIVATE /STACK:8388608)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

add_executable(postflop-solver ${PFS_SOLVER_SOURCES} src/main.cpp)
pfs_solver_target(postflop-solver)

install(TARGETS postflop-solver RUNTIME DESTINATION bin)

if(PFS_BUILD_TESTS)
  add_executable(postflop_solver_tests
    ${PFS_SOLVER_SOURCES}
    ${PROJECT_SOURCE_DIR}/tests/harness.cpp
    tests/test_solution_file.cpp
  )
  pfs_solver_target(postflop_solver_tests)
  target_include_directories(postflop_solver_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  if(MSVC)
    target_compile_definitions(postflop_solver_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME solver COMMAND postflop_solver_tests)
  set_tests_properties(solver PROPERTIES TIMEOUT 600)
endif()
