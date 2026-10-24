add_library(ghostdrive_vfs
    src/chunk_exchange.cpp
    src/fetcher.cpp
    src/file_view.cpp
    src/filesystem.cpp
    src/tree.cpp
    src/write_handle.cpp
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

# The FUSE front end needs libfuse3 from the system (Linux) or FUSE-T (macOS); without it the
# backend-independent library still builds and the mount tests are skipped.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3)
endif()
if(FUSE3_FOUND)
    add_library(ghostdrive_vfs_fuse src/fuse_mount.cpp)
    add_library(ghostdrive::vfs_fuse ALIAS ghostdrive_vfs_fuse)
    target_link_libraries(ghostdrive_vfs_fuse
        PUBLIC ghostdrive::vfs
        PRIVATE PkgConfig::FUSE3 ghostdrive_warnings)
    message(STATUS "FUSE front end: libfuse3 ${FUSE3_VERSION}")
else()
    message(STATUS "FUSE front end: libfuse3 not found, skipping (install FUSE-T on macOS)")
endif()

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
