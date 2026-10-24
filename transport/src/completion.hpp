#pragma once

#include <ngtcp2/ngtcp2.h>

#include <asio/any_io_executor.hpp>
#include <asio/append.hpp>
#include <asio/post.hpp>
#include <chrono>
#include <cstdint>
#include <utility>

namespace ghostdrive::transport::detail {

// Completes a stored handler by posting it, so a waiting coroutine never resumes inside an
// ngtcp2 callback or inside the function that completed it. Leaves `handler` empty.
template <typename Handler, typename... Args>
void post_completion(const asio::any_io_executor& executor, Handler& handler, Args&&... args) {
    if (!handler) return;
    auto taken = std::move(handler);
    handler = nullptr;
    asio::post(executor, asio::append(std::move(taken), std::forward<Args>(args)...));
}

// ngtcp2 timestamps: nanoseconds on the steady clock.
inline ngtcp2_tstamp now() noexcept {
    return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace ghostdrive::transport::detail
