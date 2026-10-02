set(PFS_TEST_SOURCES
  harness.cpp
  test_core.cpp
  test_card.cpp
  test_range.cpp
  test_bet_size.cpp
  test_action_tree.cpp
  test_kuhn.cpp
  test_leduc.cpp
  test_card_config.cpp
  test_game.cpp
  test_bunching.cpp
  test_bunching_game.cpp
  test_pio_regression.cpp
)

# save/load, and the storage-mode API it exercises, only exist when
# PFS_ENABLE_SERIALIZATION is on.
if(PFS_ENABLE_SERIALIZATION)
  list(APPEND PFS_TEST_SOURCES test_file_io.cpp)
endif()

add_executable(pfs_tests ${PFS_TEST_SOURCES})

target_link_libraries(pfs_tests PRIVATE pfs::pfs)
target_include_directories(pfs_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
  # C4324: alignas(64) padding, intentional. C4127: the landmine tests compare
  # float constants on purpose, to document that the naive spelling disagrees.
  target_compile_options(pfs_tests PRIVATE /W4 /utf-8 /bigobj /wd4324 /wd4127)
  target_compile_definitions(pfs_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  # MSVC's std::span::operator[] carries an _STL_VERIFY bounds check unless these
  # are zero. The kernels use span throughout their inner loops, so a
  # "release with checks" build would be several times slower for no reason.
  target_compile_definitions(pfs_tests PRIVATE
    $<$<CONFIG:Release>:_ITERATOR_DEBUG_LEVEL=0;_CONTAINER_DEBUG_LEVEL=0;_HAS_ITERATOR_DEBUGGING=0>)
  # The solver recurses flop -> turn -> river with fat frames, and a helping
  # worker nests its own stack on top of that.
  target_link_options(pfs_tests PRIVATE /STACK:8388608)
else()
  target_compile_options(pfs_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME unit COMMAND pfs_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 600)
