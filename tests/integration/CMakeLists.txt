add_executable(ghostdrive_integration_tests
    transport_loopback_test.cpp
    vfs_remote_read_test.cpp
    two_devices_test.cpp
    rendezvous_test.cpp
)
target_include_directories(ghostdrive_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(ghostdrive_integration_tests
    PRIVATE
        ghostdrive::transport
        ghostdrive::vfs
        ghostdrive::daemon
        ghostdrive::rendezvous
        GTest::gtest_main
        ghostdrive_warnings
)
# Test scratch files stay inside the build tree.
target_compile_definitions(ghostdrive_integration_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_integration_tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS integration TIMEOUT 120)

# macOS fills freed memory with 0x55 under MallocScribble, so a coroutine that reads a destroyed
# object dereferences garbage and crashes rather than reading stale but intact bytes.
if(APPLE)
    add_test(NAME RendezvousServer.CanBeDestroyedBeforeItsExecutorRuns.scribbled
        COMMAND ghostdrive_integration_tests
                --gtest_filter=RendezvousServer.CanBeDestroyedBeforeItsExecutorRuns)
    set_tests_properties(RendezvousServer.CanBeDestroyedBeforeItsExecutorRuns.scribbled
        PROPERTIES LABELS integration TIMEOUT 60 ENVIRONMENT "MallocScribble=1")
endif()

# End to end with the real binaries: two daemons, two FUSE or WinFsp mounts on this machine.
# On Windows the scripts run in Git Bash (not the WSL bash.exe in System32).
set(e2e_shell)
if(WIN32)
    find_program(GHOSTDRIVE_GIT_BASH bash PATHS "$ENV{ProgramFiles}/Git/bin" NO_DEFAULT_PATH)
    set(e2e_shell ${GHOSTDRIVE_GIT_BASH})
endif()
if(TARGET ghostdrive_vfs_fuse OR (TARGET ghostdrive_vfs_winfsp AND GHOSTDRIVE_GIT_BASH))
    add_test(NAME e2e.two_devices
        COMMAND ${e2e_shell} ${PROJECT_SOURCE_DIR}/tests/e2e/two_devices.sh ${PROJECT_BINARY_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/e2e)
    set_tests_properties(e2e.two_devices PROPERTIES
        LABELS "e2e;mount" RUN_SERIAL TRUE TIMEOUT 180 PASS_REGULAR_EXPRESSION "PASS")
    add_test(NAME e2e.via_rendezvous
        COMMAND ${e2e_shell} ${PROJECT_SOURCE_DIR}/tests/e2e/via_rendezvous.sh ${PROJECT_BINARY_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/e2e-rendezvous)
    set_tests_properties(e2e.via_rendezvous PROPERTIES
        LABELS "e2e;mount" RUN_SERIAL TRUE TIMEOUT 180 PASS_REGULAR_EXPRESSION "PASS")
endif()
