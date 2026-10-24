add_executable(ghostdrive_integration_tests
    transport_loopback_test.cpp
    vfs_remote_read_test.cpp
)
target_include_directories(ghostdrive_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(ghostdrive_integration_tests
    PRIVATE
        ghostdrive::transport
        ghostdrive::vfs
        GTest::gtest_main
        ghostdrive_warnings
)
# Test scratch files stay inside the build tree.
target_compile_definitions(ghostdrive_integration_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_integration_tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS integration TIMEOUT 120)
