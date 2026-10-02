add_executable(postflop-solver
  src/game_file.hpp
  src/game_file.cpp
  src/gto_range.hpp
  src/gto_range.cpp
  src/result_file.hpp
  src/result_file.cpp
  src/main.cpp
)
target_link_libraries(postflop-solver PRIVATE pfs::pfs)
target_include_directories(postflop-solver PRIVATE ${PROJECT_SOURCE_DIR}/src)
# Third party nlohmann/json (header only), included as <nlohmann/json.hpp>.
target_include_directories(postflop-solver SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third-party)

if(MSVC)
  target_compile_options(postflop-solver PRIVATE /W4 /utf-8 /wd4324)
  # std::span bounds checks in the solver's inline kernels would slow a Release build down.
  target_compile_definitions(postflop-solver PRIVATE
    $<$<CONFIG:Release>:_ITERATOR_DEBUG_LEVEL=0;_CONTAINER_DEBUG_LEVEL=0;_HAS_ITERATOR_DEBUGGING=0>)
  # solve_recursive nests deeply, and a helping worker stacks on top of that.
  target_link_options(postflop-solver PRIVATE /STACK:8388608)
else()
  target_compile_options(postflop-solver PRIVATE -Wall -Wextra)
endif()

install(TARGETS postflop-solver RUNTIME DESTINATION bin)
