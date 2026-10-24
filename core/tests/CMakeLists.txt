add_executable(ghostdrive_core_tests
    bytes_test.cpp
    chunk_id_test.cpp
    chunk_store_test.cpp
    chunking_test.cpp
    file_record_test.cpp
    hlc_test.cpp
    identity_test.cpp
    metadata_index_test.cpp
    version_vector_test.cpp
)
target_link_libraries(ghostdrive_core_tests
    PRIVATE
        ghostdrive::core
        GTest::gtest_main
        ghostdrive_warnings
)
# Test scratch files stay inside the build tree.
target_compile_definitions(ghostdrive_core_tests
    PRIVATE GHOSTDRIVE_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

gtest_discover_tests(ghostdrive_core_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
