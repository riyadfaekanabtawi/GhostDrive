add_executable(ghostdrive_cli_tests autostart_test.cpp)
target_include_directories(ghostdrive_cli_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(ghostdrive_cli_tests
    PRIVATE
        ghostdrive_autostart
        GTest::gtest_main
        ghostdrive_warnings
)
target_compile_definitions(ghostdrive_cli_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_cli_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
