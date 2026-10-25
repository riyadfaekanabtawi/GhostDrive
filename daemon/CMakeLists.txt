add_library(ghostdrive_daemon
    src/codec.cpp
    src/control.cpp
    src/discovery_keys.cpp
    src/drive.cpp
    src/invite.cpp
    src/membership.cpp
    src/service.cpp
)
add_library(ghostdrive::daemon ALIAS ghostdrive_daemon)

target_include_directories(ghostdrive_daemon PUBLIC include PRIVATE src)
target_link_libraries(ghostdrive_daemon
    PUBLIC
        ghostdrive::vfs
        ghostdrive::discovery
        ghostdrive::transport
        ghostdrive::core
    PRIVATE
        OpenSSL::Crypto
        ghostdrive_warnings
)
if(WIN32)
    # iphlpapi: GetAdaptersAddresses; ws2_32: WSAIoctl for the control socket's peer.
    target_link_libraries(ghostdrive_daemon PRIVATE iphlpapi ws2_32)
endif()

add_executable(ghostdrived ghostdrived.cpp)
target_include_directories(ghostdrived PRIVATE src)
target_link_libraries(ghostdrived PRIVATE ghostdrive::daemon ghostdrive_version ghostdrive_warnings)
if(TARGET ghostdrive_vfs_fuse)
    target_link_libraries(ghostdrived PRIVATE ghostdrive::vfs_fuse)
    target_compile_definitions(ghostdrived PRIVATE GHOSTDRIVE_HAVE_FUSE=1)
endif()

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
