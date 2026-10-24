#include <ngtcp2/ngtcp2.h>

#include <algorithm>
#include <asio/async_result.hpp>
#include <asio/use_awaitable.hpp>
#include <cstring>

#include "completion.hpp"
#include "ghostdrive/transport/endpoint.hpp"

namespace ghostdrive::transport {

using core::Errc;
using core::Error;
using detail::post_completion;

Stream::Stream(Private, std::weak_ptr<Connection> conn, asio::any_io_executor executor,
               std::int64_t id)
    : conn_(std::move(conn)), executor_(std::move(executor)), id_(id) {}

Stream::~Stream() = default;

asio::awaitable<Result<std::size_t>> Stream::read(std::span<std::byte> out) {
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>,
                                            void(Result<std::size_t>)>(
        [this](auto handler, std::span<std::byte> buffer) {
            start_read(ReadHandler(std::move(handler)), buffer);
        },
        asio::use_awaitable, out);
}

asio::awaitable<Status> Stream::write(std::span<const std::span<const std::byte>> buffers,
                                      bool fin) {
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>, void(Status)>(
        [this](auto handler, std::span<const std::span<const std::byte>> bufs, bool f) {
            start_write(WriteHandler(std::move(handler)), bufs, f);
        },
        asio::use_awaitable, buffers, fin);
}

asio::awaitable<Status> Stream::write(std::span<const std::byte> data, bool fin) {
    const std::span<const std::byte> one[] = {data};
    co_return co_await write(std::span<const std::span<const std::byte>>(one), fin);
}

void Stream::start_read(ReadHandler handler, std::span<std::byte> out) {
    if (read_handler_) {
        post_completion(executor_, handler,
                        Result<std::size_t>(Error{Errc::invalid_argument, "read already pending"}));
        return;
    }
    if (out.empty()) {
        post_completion(executor_, handler,
                        Result<std::size_t>(Error{Errc::invalid_argument, "empty read buffer"}));
        return;
    }
    std::size_t available = rx_.size() - rx_head_;
    if (available > 0) {
        std::size_t n = std::min(available, out.size());
        // COPY: buffered stream bytes go to the caller's buffer; they arrived while no read was
        // waiting.
        std::memcpy(out.data(), rx_.data() + rx_head_, n);
        rx_head_ += n;
        if (rx_head_ == rx_.size()) {
            rx_.clear();
            rx_head_ = 0;
        } else if (rx_head_ >= rx_.size() / 2) {
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(rx_head_));
            rx_head_ = 0;
        }
        give_credit(n);
        post_completion(executor_, handler, Result<std::size_t>(n));
        return;
    }
    if (rx_fin_) {
        post_completion(executor_, handler, Result<std::size_t>(std::size_t{0}));
        return;
    }
    if (error_) {
        post_completion(executor_, handler, Result<std::size_t>(*error_));
        return;
    }
    read_handler_ = std::move(handler);
    read_out_ = out;
}

void Stream::start_write(WriteHandler handler, std::span<const std::span<const std::byte>> buffers,
                         bool fin) {
    if (write_handler_) {
        post_completion(executor_, handler,
                        Status(Error{Errc::invalid_argument, "write already pending"}));
        return;
    }
    if (error_) {
        post_completion(executor_, handler, Status(*error_));
        return;
    }
    if (fin_pending_) {
        post_completion(executor_, handler,
                        Status(Error{Errc::invalid_argument, "write after end of stream"}));
        return;
    }
    auto conn = conn_.lock();
    if (!conn || conn->closed_) {
        post_completion(executor_, handler, Status(Error{Errc::closed, "connection closed"}));
        return;
    }

    tx_buffers_.clear();
    std::uint64_t total = 0;
    for (auto buffer : buffers) {
        if (buffer.empty()) continue;
        tx_buffers_.push_back(buffer);
        total += buffer.size();
    }
    tx_base_ = tx_end_;
    tx_end_ += total;
    fin_pending_ = fin;
    write_handler_ = std::move(handler);

    if (has_unsent() || wants_fin()) {
        conn->enqueue_send(id_);
        conn->schedule_flush();
    }
    maybe_complete_write();
}

void Stream::reset(std::uint64_t app_error_code) {
    auto conn = conn_.lock();
    if (conn && !conn->closed_ && conn->conn_ != nullptr) {
        ngtcp2_conn_shutdown_stream(conn->conn_, 0, id_, app_error_code);
        conn->schedule_flush();
    }
    on_closed(Error{Errc::closed, "stream reset locally"});
}

void Stream::on_data(std::span<const std::byte> data, bool fin) {
    std::size_t used = 0;
    if (read_handler_ && !data.empty()) {
        used = std::min(data.size(), read_out_.size());
        // COPY: ngtcp2 stream data is only valid during the callback (C7).
        std::memcpy(read_out_.data(), data.data(), used);
        give_credit(used);
        read_out_ = {};
        post_completion(executor_, read_handler_, Result<std::size_t>(used));
    }
    if (used < data.size()) {
        // COPY: same reason as above, for bytes that arrive before a read asks for them. Flow
        // control bounds this buffer: credit returns only as the reader consumes bytes.
        rx_.insert(rx_.end(), data.begin() + static_cast<std::ptrdiff_t>(used), data.end());
    }
    if (fin) {
        rx_fin_ = true;
        if (read_handler_) {
            read_out_ = {};
            post_completion(executor_, read_handler_, Result<std::size_t>(std::size_t{0}));
        }
    }
}

void Stream::on_acked(std::uint64_t offset, std::uint64_t length) {
    tx_acked_ = std::max(tx_acked_, offset + length);
    maybe_complete_write();
}

void Stream::on_sent(std::size_t length, bool fin_flagged) {
    tx_sent_ += length;
    if (fin_flagged && fin_pending_ && tx_sent_ == tx_end_) fin_sent_ = true;
    maybe_complete_write();
}

void Stream::on_closed(core::Error reason) {
    if (!error_) error_ = std::move(reason);
    if (read_handler_) {
        read_out_ = {};
        if (rx_fin_) {
            post_completion(executor_, read_handler_, Result<std::size_t>(std::size_t{0}));
        } else {
            post_completion(executor_, read_handler_, Result<std::size_t>(*error_));
        }
    }
    maybe_complete_write();
    if (write_handler_) {
        tx_buffers_.clear();
        post_completion(executor_, write_handler_, Status(*error_));
    }
}

bool Stream::has_unsent() const noexcept {
    return tx_sent_ < tx_end_;
}

std::size_t Stream::unsent_vectors(std::span<std::span<const std::byte>> out) const {
    std::uint64_t skip = tx_sent_ - tx_base_;
    std::size_t count = 0;
    for (auto buffer : tx_buffers_) {
        if (count == out.size()) break;
        if (skip >= buffer.size()) {
            skip -= buffer.size();
            continue;
        }
        out[count++] = buffer.subspan(static_cast<std::size_t>(skip));
        skip = 0;
    }
    return count;
}

void Stream::give_credit(std::size_t length) {
    auto conn = conn_.lock();
    if (!conn || conn->closed_ || conn->conn_ == nullptr || length == 0) return;
    ngtcp2_conn_extend_max_stream_offset(conn->conn_, id_, length);
    ngtcp2_conn_extend_max_offset(conn->conn_, length);
    conn->schedule_flush();
}

void Stream::maybe_complete_write() {
    if (!write_handler_) return;
    bool data_done = tx_acked_ >= tx_end_;
    bool fin_done = !fin_pending_ || fin_sent_;
    if (data_done && fin_done) {
        tx_buffers_.clear();
        post_completion(executor_, write_handler_, Status{});
    }
}

}  // namespace ghostdrive::transport
