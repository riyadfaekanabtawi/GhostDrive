# GhostDrive's Linux desktop app (GTK 4 and libadwaita). The logic library and its tests have
# no GTK dependency and are part of every build; the app itself needs
# -DGHOSTDRIVE_BUILD_LINUX_APP=ON and the gtk4 and libadwaita-1 development packages.

add_library(ghostdrive_app_logic STATIC
    logic/src/cli.cpp
    logic/src/json.cpp
    logic/src/status.cpp
    logic/src/text.cpp
)
target_include_directories(ghostdrive_app_logic PUBLIC logic/include)
target_link_libraries(ghostdrive_app_logic PRIVATE ghostdrive_warnings)

if(GHOSTDRIVE_BUILD_TESTS)
    add_executable(ghostdrive_app_logic_tests
        logic/tests/cli_test.cpp
        logic/tests/status_test.cpp
        logic/tests/text_test.cpp
    )
    target_link_libraries(ghostdrive_app_logic_tests
        PRIVATE ghostdrive_app_logic GTest::gtest_main ghostdrive_warnings)
    gtest_discover_tests(ghostdrive_app_logic_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

    # The parser against the daemon's own status writer.
    add_executable(ghostdrive_app_contract_tests logic/tests/contract_test.cpp)
    target_include_directories(ghostdrive_app_contract_tests PRIVATE ${PROJECT_SOURCE_DIR}/daemon/src)
    target_link_libraries(ghostdrive_app_contract_tests
        PRIVATE ghostdrive_app_logic ghostdrive::daemon GTest::gtest_main ghostdrive_warnings)
    gtest_discover_tests(ghostdrive_app_contract_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
endif()

if(NOT GHOSTDRIVE_BUILD_LINUX_APP)
    return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.5)

add_executable(ghostdrive-app
    src/main.cpp
    src/runner.cpp
    src/window.cpp
)
target_link_libraries(ghostdrive-app
    PRIVATE ghostdrive_app_logic PkgConfig::LIBADWAITA PkgConfig::GTK4 ghostdrive_warnings)

# The icon next to the binary, so a build-tree run finds it (main.cpp adds this search path).
configure_file(data/org.ghostdrive.GhostDrive.svg
    ${CMAKE_CURRENT_BINARY_DIR}/icons/hicolor/scalable/apps/org.ghostdrive.GhostDrive.svg COPYONLY)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(GHOSTDRIVE_BUILD_TESTS AND DESKTOP_FILE_VALIDATE)
    add_test(NAME app.desktop_file
             COMMAND ${DESKTOP_FILE_VALIDATE} ${CMAKE_CURRENT_SOURCE_DIR}/data/org.ghostdrive.GhostDrive.desktop)
    set_tests_properties(app.desktop_file PROPERTIES LABELS unit)
endif()

install(TARGETS ghostdrive-app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/org.ghostdrive.GhostDrive.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/org.ghostdrive.GhostDrive.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
