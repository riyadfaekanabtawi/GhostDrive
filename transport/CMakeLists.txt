add_library(ghostdrive_transport
    src/connection.cpp
    src/demux.cpp
    src/endpoint.cpp
    src/frame.cpp
    src/impairment.cpp
    src/stream.cpp
    src/tls.cpp
)
add_library(ghostdrive::transport ALIAS ghostdrive_transport)

# vcpkg builds ngtcp2 as static libraries on macOS and Linux, and as DLLs for x64-windows.
if(TARGET ngtcp2::ngtcp2_static)
    set(ghostdrive_ngtcp2 ngtcp2::ngtcp2_crypto_ossl_static ngtcp2::ngtcp2_static)
else()
    set(ghostdrive_ngtcp2 ngtcp2::ngtcp2_crypto_ossl ngtcp2::ngtcp2)
endif()

target_include_directories(ghostdrive_transport PUBLIC include PRIVATE src)
target_link_libraries(ghostdrive_transport
    PUBLIC
        ghostdrive::core
        asio::asio
    PRIVATE
        ${ghostdrive_ngtcp2}
        OpenSSL::SSL
        OpenSSL::Crypto
        ghostdrive_warnings
)

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
