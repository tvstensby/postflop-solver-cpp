foreach(name basic node_locking file_io)
  # file_io needs the serialization support.
  if(name STREQUAL "file_io" AND NOT PFS_ENABLE_SERIALIZATION)
    continue()
  endif()

  add_executable(example_${name} ${name}.cpp)
  target_link_libraries(example_${name} PRIVATE pfs::pfs)
  target_include_directories(example_${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)

  if(MSVC)
    target_compile_options(example_${name} PRIVATE /W4 /utf-8 /wd4324)
    # The examples assert, so keep them enabled even in Release.
    target_compile_definitions(example_${name} PRIVATE
      $<$<CONFIG:Release,RelWithDebInfo>:_ITERATOR_DEBUG_LEVEL=0>)
    # solve_recursive nests deeply, and a helping worker stacks on top of that.
    target_link_options(example_${name} PRIVATE /STACK:8388608)
  else()
    target_compile_options(example_${name} PRIVATE -Wall -Wextra)
  endif()

  # The examples carry the assertions from the Rust originals, so run them as tests.
  if(PFS_BUILD_TESTS)
    add_test(NAME example_${name} COMMAND example_${name})
    set_tests_properties(example_${name} PROPERTIES TIMEOUT 1800 LABELS slow)
  endif()
endforeach()
