add_library(ghostdrive_core
    src/bytes.cpp
    src/chunk_id.cpp
    src/chunk_store.cpp
    src/chunking.cpp
    src/file_record.cpp
    src/hlc.cpp
    src/identity.cpp
    src/metadata_index.cpp
    src/posix_file.cpp
    src/version_vector.cpp
)
add_library(ghostdrive::core ALIAS ghostdrive_core)

target_include_directories(ghostdrive_core PUBLIC include)
target_link_libraries(ghostdrive_core
    PRIVATE
        BLAKE3::blake3
        OpenSSL::Crypto
        unofficial::sqlite3::sqlite3
        ghostdrive_warnings
)

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
