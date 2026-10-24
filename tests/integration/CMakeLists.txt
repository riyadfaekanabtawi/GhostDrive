add_executable(ghostdrive_integration_tests
    transport_loopback_test.cpp
    vfs_remote_read_test.cpp
    two_devices_test.cpp
)
target_include_directories(ghostdrive_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(ghostdrive_integration_tests
    PRIVATE
        ghostdrive::transport
        ghostdrive::vfs
        ghostdrive::daemon
        GTest::gtest_main
        ghostdrive_warnings
)
# Test scratch files stay inside the build tree.
target_compile_definitions(ghostdrive_integration_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_integration_tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS integration TIMEOUT 120)

# End to end with the real binaries: two daemons, two FUSE mounts on this machine.
if(TARGET ghostdrive_vfs_fuse)
    add_test(NAME e2e.two_devices
        COMMAND ${PROJECT_SOURCE_DIR}/tests/e2e/two_devices.sh ${PROJECT_BINARY_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/e2e)
    set_tests_properties(e2e.two_devices PROPERTIES
        LABELS "e2e;mount" RUN_SERIAL TRUE TIMEOUT 180 PASS_REGULAR_EXPRESSION "PASS")
endif()
