#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/append.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>
#include <type_traits>
#include <utility>

namespace ghostdrive::daemon::detail {

// Completes a stored handler by posting it, leaving `handler` empty.
template <typename Handler, typename... Args>
void post_completion(const asio::any_io_executor& executor, Handler& handler, Args&&... args) {
    if (!handler) return;
    auto taken = std::move(handler);
    handler = nullptr;
    asio::post(executor, asio::append(std::move(taken), std::forward<Args>(args)...));
}

// Runs a blocking function on `executor` (the disk pool) and resumes the caller on its own
// executor with the result.
template <typename F>
asio::awaitable<std::invoke_result_t<F>> run_on(asio::any_io_executor executor, F f) {
    using R = std::invoke_result_t<F>;
    co_return co_await asio::async_initiate<const asio::use_awaitable_t<>, void(R)>(
        [executor](auto handler, F fn) {
            asio::post(executor, [fn = std::move(fn), handler = std::move(handler)]() mutable {
                R result = fn();
                auto resume_on = asio::get_associated_executor(handler);
                asio::post(resume_on, asio::append(std::move(handler), std::move(result)));
            });
        },
        asio::use_awaitable, std::move(f));
}

}  // namespace ghostdrive::daemon::detail
