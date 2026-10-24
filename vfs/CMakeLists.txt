add_library(ghostdrive_vfs
    src/chunk_exchange.cpp
    src/fetcher.cpp
    src/file_view.cpp
    src/filesystem.cpp
    src/tree.cpp
)
add_library(ghostdrive::vfs ALIAS ghostdrive_vfs)

target_include_directories(ghostdrive_vfs PUBLIC include PRIVATE src)
target_link_libraries(ghostdrive_vfs
    PUBLIC
        ghostdrive::core
        ghostdrive::transport
    PRIVATE
        ghostdrive_warnings
)

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
