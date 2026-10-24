add_executable(ghostdrive_daemon_tests
    codec_test.cpp
    drive_test.cpp
    invite_test.cpp
    membership_test.cpp
)
target_include_directories(ghostdrive_daemon_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(ghostdrive_daemon_tests
    PRIVATE
        ghostdrive::daemon
        GTest::gtest_main
        ghostdrive_warnings
)
target_compile_definitions(ghostdrive_daemon_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_daemon_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
