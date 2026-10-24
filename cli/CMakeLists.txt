add_executable(ghostdrive ghostdrive.cpp)
target_link_libraries(ghostdrive PRIVATE ghostdrive::daemon ghostdrive_warnings)
