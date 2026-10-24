add_executable(ghostdrive_discovery_tests lan_id_test.cpp)
target_link_libraries(ghostdrive_discovery_tests
    PRIVATE ghostdrive::discovery GTest::gtest_main ghostdrive_warnings)
gtest_discover_tests(ghostdrive_discovery_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
