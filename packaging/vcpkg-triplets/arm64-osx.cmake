# vcpkg's arm64-osx triplet with a minimum macOS version, used by scripts/package.sh so release
# builds run on macOS 13 and later, not only on the build machine's version. CMake-based ports
# get CMAKE_OSX_DEPLOYMENT_TARGET from this; the others (OpenSSL) read the environment variable.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.0)
set(VCPKG_ENV_PASSTHROUGH MACOSX_DEPLOYMENT_TARGET)
