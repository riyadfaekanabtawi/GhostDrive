# autostart: a launchd agent, a systemd user unit or a Task Scheduler logon task.
add_library(ghostdrive_autostart autostart.cpp)
target_include_directories(ghostdrive_autostart PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ghostdrive_autostart
    PUBLIC ghostdrive::core
    PRIVATE BLAKE3::blake3 ghostdrive_warnings)
if(WIN32)
    target_sources(ghostdrive_autostart PRIVATE autostart_windows.cpp)
    # taskschd: the Task Scheduler's COM class and interface IDs.
    target_link_libraries(ghostdrive_autostart PRIVATE taskschd ole32 oleaut32 advapi32)
endif()

add_executable(ghostdrive ghostdrive.cpp)
if(WIN32)
    # UTF-8 as the process code page, so arguments and the environment keep any file name.
    target_sources(ghostdrive PRIVATE ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest)
endif()
target_link_libraries(ghostdrive
    PRIVATE ghostdrive::daemon ghostdrive_autostart ghostdrive_version ghostdrive_warnings)

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
