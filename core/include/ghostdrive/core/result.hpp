#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace ghostdrive::core {

enum class Errc {
    io,
    not_found,
    already_exists,
    invalid_argument,
    corrupt,
    crypto,
    database,
    permission,
    closed,
    timeout,
    protocol,
};

struct Error {
    Errc code;
    std::string message;
};

// Value-or-error return type. Core code does not throw; the C ABI maps Error to error codes.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : state_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & {
        assert(ok());
        return *std::get_if<0>(&state_);
    }
    const T& value() const& {
        assert(ok());
        return *std::get_if<0>(&state_);
    }
    T&& value() && {
        assert(ok());
        return std::move(*std::get_if<0>(&state_));
    }
    const Error& error() const {
        assert(!ok());
        return *std::get_if<1>(&state_);
    }

private:
    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const {
        assert(!ok());
        return *error_;
    }

private:
    std::optional<Error> error_;
};

using Status = Result<void>;

}  // namespace ghostdrive::core
