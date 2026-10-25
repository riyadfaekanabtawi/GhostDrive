add_library(ghostdrive_core
    src/bytes.cpp
    src/chunk_cache.cpp
    src/chunk_id.cpp
    src/chunk_store.cpp
    src/chunking.cpp
    src/file.cpp
    src/file_record.cpp
    src/hlc.cpp
    src/identity.cpp
    src/metadata_index.cpp
    src/version_vector.cpp
)
add_library(ghostdrive::core ALIAS ghostdrive_core)
if(WIN32)
    target_sources(ghostdrive_core PRIVATE src/file_win32.cpp)
    target_link_libraries(ghostdrive_core PRIVATE advapi32)
else()
    target_sources(ghostdrive_core PRIVATE src/file_posix.cpp)
endif()

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
