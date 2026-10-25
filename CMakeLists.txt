cmake_minimum_required(VERSION 3.25)
project(ghostdrive VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GHOSTDRIVE_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list, e.g. address,undefined")
option(GHOSTDRIVE_WERROR "Treat warnings as errors" OFF)
option(GHOSTDRIVE_BUILD_TESTS "Build tests" ON)

include(GNUInstallDirs)
# FUSE-T's libfuse3 has the install name @rpath/libfuse3.4.dylib, so installed programs need
# the rpath of the directory they were linked against (for example /usr/local/lib).
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

# ghostdrive/version.hpp carries the project() version for the --version output.
configure_file(cmake/version.hpp.in
               ${PROJECT_BINARY_DIR}/generated/include/ghostdrive/version.hpp @ONLY)
add_library(ghostdrive_version INTERFACE)
target_include_directories(ghostdrive_version INTERFACE ${PROJECT_BINARY_DIR}/generated/include)

add_library(ghostdrive_warnings INTERFACE)
# clang-cl reads -Wall as /Wall, which is -Weverything; /W4 is its -Wall -Wextra.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(ghostdrive_warning_level /W4)
else()
    set(ghostdrive_warning_level -Wall -Wextra)
endif()
target_compile_options(ghostdrive_warnings INTERFACE
    ${ghostdrive_warning_level} -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    $<$<BOOL:${GHOSTDRIVE_WERROR}>:-Werror>)

if(WIN32)
    # Windows 10 is the minimum: it has AF_UNIX sockets, used by the control socket.
    add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(GHOSTDRIVE_SANITIZERS)
    add_compile_options(-fsanitize=${GHOSTDRIVE_SANITIZERS} -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${GHOSTDRIVE_SANITIZERS})
endif()

find_package(OpenSSL REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(ngtcp2 CONFIG REQUIRED)

if(GHOSTDRIVE_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()
add_subdirectory(core)
add_subdirectory(transport)
add_subdirectory(vfs)
add_subdirectory(discovery)
add_subdirectory(rendezvous)
add_subdirectory(daemon)
add_subdirectory(cli)
if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests/integration)
endif()

# Only the three programs and the license are installed: the libraries are static and internal,
# and test binaries stay in the build tree. ghostdrive and ghostdrived share a directory, which
# is where `ghostdrive autostart on` looks for the daemon.
install(TARGETS ghostdrive ghostdrived ghostdrive-rendezvous
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
