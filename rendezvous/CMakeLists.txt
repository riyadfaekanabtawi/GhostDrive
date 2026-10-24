add_library(ghostdrive_rendezvous src/server.cpp)
add_library(ghostdrive::rendezvous ALIAS ghostdrive_rendezvous)
target_include_directories(ghostdrive_rendezvous PUBLIC include)
target_link_libraries(ghostdrive_rendezvous
    PUBLIC ghostdrive::discovery ghostdrive::transport ghostdrive::core
    PRIVATE ghostdrive_warnings)

add_executable(ghostdrive-rendezvous ghostdrive-rendezvous.cpp)
target_link_libraries(ghostdrive-rendezvous PRIVATE ghostdrive::rendezvous ghostdrive_warnings)
