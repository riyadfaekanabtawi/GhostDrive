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

target_include_directories(ghostdrive_transport PUBLIC include PRIVATE src)
target_link_libraries(ghostdrive_transport
    PUBLIC
        ghostdrive::core
        asio::asio
    PRIVATE
        ngtcp2::ngtcp2_crypto_ossl_static
        ngtcp2::ngtcp2_static
        OpenSSL::SSL
        OpenSSL::Crypto
        ghostdrive_warnings
)

if(GHOSTDRIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
