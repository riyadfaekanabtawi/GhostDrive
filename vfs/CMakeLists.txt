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

# The WinFsp front end needs the WinFsp SDK (the installer's "Developer" feature, in
# %ProgramFiles(x86)%\WinFsp, or WINFSP_ROOT). winfsp-x64.dll is delay-loaded from the
# installation at mount time, so ghostdrived still starts without WinFsp (with --no-mount).
if(WIN32)
    set(program_files_x86 "ProgramFiles(x86)")
    set(winfsp_hints "${WINFSP_ROOT}" "$ENV{WINFSP_ROOT}" "$ENV{${program_files_x86}}/WinFsp")
    find_path(WINFSP_INCLUDE_DIR winfsp/winfsp.h HINTS ${winfsp_hints} PATH_SUFFIXES inc)
    find_library(WINFSP_LIBRARY winfsp-x64 HINTS ${winfsp_hints} PATH_SUFFIXES lib)
    if(WINFSP_INCLUDE_DIR AND WINFSP_LIBRARY)
        add_library(ghostdrive_vfs_winfsp src/winfsp_mount.cpp)
        add_library(ghostdrive::vfs_winfsp ALIAS ghostdrive_vfs_winfsp)
        target_include_directories(ghostdrive_vfs_winfsp SYSTEM PRIVATE ${WINFSP_INCLUDE_DIR})
        target_link_libraries(ghostdrive_vfs_winfsp
            PUBLIC ghostdrive::vfs
            PRIVATE ${WINFSP_LIBRARY} delayimp ghostdrive_warnings)
        target_link_options(ghostdrive_vfs_winfsp INTERFACE /DELAYLOAD:winfsp-x64.dll)
        message(STATUS "WinFsp front end: ${WINFSP_LIBRARY}")
    else()
        message(STATUS "WinFsp front end: WinFsp SDK not found, skipping (install WinFsp)")
    endif()
endif()

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
