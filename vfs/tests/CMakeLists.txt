add_executable(ghostdrive_vfs_tests
    file_view_test.cpp
    tree_test.cpp
    write_test.cpp
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

if(TARGET ghostdrive_vfs_fuse)
    add_executable(ghostdrive_mount_tests mount_test.cpp)
    target_include_directories(ghostdrive_mount_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
    target_link_libraries(ghostdrive_mount_tests
        PRIVATE
            ghostdrive::vfs_fuse
            GTest::gtest_main
            ghostdrive_warnings
    )
    target_compile_definitions(ghostdrive_mount_tests
        PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
    # Real mounts: run serially, and skip with `ctest -LE mount` where FUSE is unusable.
    gtest_discover_tests(ghostdrive_mount_tests DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS mount RUN_SERIAL TRUE TIMEOUT 120)
endif()
