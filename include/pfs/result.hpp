// Rust's `Result<T, String>`, which is what the whole crate uses for
// configuration and parsing errors (there is no custom error type anywhere).
// C++20 has no std::expected, so this is a minimal stand-in with the same shape.
//
// Note the split the Rust makes and this port preserves: construction and
// parsing return Result, while the post-solve interpreter API *panics* on misuse
// (PFS_PANIC). That is deliberate -- the ported tests observe both.
#pragma once

#include <pfs/common.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace pfs {

struct Error {
    std::string message;
};

template <class T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error e) : v_(std::move(e)) {}

    static Result err(std::string msg) { return Result(Error{std::move(msg)}); }

    bool is_ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return is_ok(); }

    // Precondition: is_ok(). Rust's `.unwrap()`, and it panics the same way.
    T& value() {
        if (!is_ok()) PFS_PANIC(("unwrap on Err: " + error()).c_str());
        return std::get<0>(v_);
    }
    const T& value() const {
        if (!is_ok()) PFS_PANIC(("unwrap on Err: " + error()).c_str());
        return std::get<0>(v_);
    }

    const std::string& error() const { return std::get<1>(v_).message; }

private:
    std::variant<T, Error> v_;
};

// Rust's `Result<(), String>`.
class Status {
public:
    Status() = default;
    Status(Error e) : err_(std::move(e.message)) {}

    static Status ok() { return Status(); }
    static Status err(std::string msg) { return Status(Error{std::move(msg)}); }

    bool is_ok() const noexcept { return !err_.has_value(); }
    explicit operator bool() const noexcept { return is_ok(); }
    const std::string& error() const { return *err_; }

private:
    std::optional<std::string> err_;
};

}  // namespace pfs
