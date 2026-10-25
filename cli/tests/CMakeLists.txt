if(TARGET ghostdrive_autostart)
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
endif()

# The output must be exactly the project() version; the Homebrew formula's test relies on it.
string(REPLACE "." "\\." version_regex "${PROJECT_VERSION}")
add_test(NAME cli.version COMMAND ghostdrive --version)
set_tests_properties(cli.version PROPERTIES
    LABELS unit PASS_REGULAR_EXPRESSION "^ghostdrive ${version_regex}\r?\n$")
