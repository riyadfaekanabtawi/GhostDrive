#pragma once

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <openssl/x509.h>

namespace ghostdrive::transport {

// ngtcp2 and OpenSSL callbacks. They only record events on the Connection; anything that
// resumes a coroutine is posted (see Connection::after_events).
struct Callbacks {
    static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* ref);
    static void rand(std::uint8_t* dest, std::size_t len, const ngtcp2_rand_ctx* ctx);
    static int get_new_connection_id(ngtcp2_conn* conn, ngtcp2_cid* cid,
                                     ngtcp2_stateless_reset_token* token, std::size_t cidlen,
                                     void* user_data);
    static int remove_connection_id(ngtcp2_conn* conn, const ngtcp2_cid* cid, void* user_data);
    static int handshake_completed(ngtcp2_conn* conn, void* user_data);
    static int handshake_confirmed(ngtcp2_conn* conn, void* user_data);
    static int recv_stream_data(ngtcp2_conn* conn, std::uint32_t flags, std::int64_t stream_id,
                                std::uint64_t offset, const std::uint8_t* data, std::size_t datalen,
                                void* user_data, void* stream_user_data);
    static int stream_open(ngtcp2_conn* conn, std::int64_t stream_id, void* user_data);
    static int acked_stream_data_offset(ngtcp2_conn* conn, std::int64_t stream_id,
                                        std::uint64_t offset, std::uint64_t datalen,
                                        void* user_data, void* stream_user_data);
    static int stream_close(ngtcp2_conn* conn, std::uint32_t flags, std::int64_t stream_id,
                            std::uint64_t rx_app_error_code, std::uint64_t tx_app_error_code,
                            void* user_data, void* stream_user_data);
    static int stream_reset(ngtcp2_conn* conn, std::int64_t stream_id, std::uint64_t final_size,
                            std::uint64_t app_error_code, void* user_data, void* stream_user_data);
    static int recv_datagram(ngtcp2_conn* conn, std::uint32_t flags, const std::uint8_t* data,
                             std::size_t datalen, void* user_data);
    static int extend_max_local_streams_bidi(ngtcp2_conn* conn, std::uint64_t max_streams,
                                             void* user_data);

    // OpenSSL certificate verification: decides on the leaf by its pinned key.
    static int verify_certificate(int preverify_ok, X509_STORE_CTX* store);

    static const ngtcp2_callbacks client;
    static const ngtcp2_callbacks server;
};

}  // namespace ghostdrive::transport
