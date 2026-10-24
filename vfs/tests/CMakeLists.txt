add_executable(ghostdrive_vfs_tests
    file_view_test.cpp
    tree_test.cpp
)
target_include_directories(ghostdrive_vfs_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(ghostdrive_vfs_tests
    PRIVATE
        ghostdrive::vfs
        GTest::gtest_main
        ghostdrive_warnings
)
# Test scratch files stay inside the build tree.
target_compile_definitions(ghostdrive_vfs_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_vfs_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

