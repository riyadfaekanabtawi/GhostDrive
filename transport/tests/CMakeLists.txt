add_executable(ghostdrive_transport_tests
    demux_test.cpp
    frame_test.cpp
    impairment_test.cpp
    tls_test.cpp
)
target_include_directories(ghostdrive_transport_tests PRIVATE ../src)
target_link_libraries(ghostdrive_transport_tests
    PRIVATE
        ghostdrive::transport
        OpenSSL::SSL
        GTest::gtest_main
        ghostdrive_warnings
)

gtest_discover_tests(ghostdrive_transport_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
