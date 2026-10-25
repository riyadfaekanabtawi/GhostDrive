add_library(ghostdrive_autostart autostart.cpp)
target_include_directories(ghostdrive_autostart PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ghostdrive_autostart
    PUBLIC ghostdrive::core
    PRIVATE BLAKE3::blake3 ghostdrive_warnings)

add_executable(ghostdrive ghostdrive.cpp)
target_link_libraries(ghostdrive PRIVATE ghostdrive::daemon ghostdrive_autostart ghostdrive_warnings)

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
