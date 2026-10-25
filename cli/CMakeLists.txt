# autostart (launchd and systemd login items) is not built on Windows yet; `ghostdrive autostart`
# reports that instead.
if(NOT WIN32)
    add_library(ghostdrive_autostart autostart.cpp)
    target_include_directories(ghostdrive_autostart PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ghostdrive_autostart
        PUBLIC ghostdrive::core
        PRIVATE BLAKE3::blake3 ghostdrive_warnings)
endif()

add_executable(ghostdrive ghostdrive.cpp)
target_link_libraries(ghostdrive
    PRIVATE ghostdrive::daemon ghostdrive_version ghostdrive_warnings)
if(TARGET ghostdrive_autostart)
    target_link_libraries(ghostdrive PRIVATE ghostdrive_autostart)
endif()

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
