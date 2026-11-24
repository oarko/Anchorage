// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <cstdlib>
#include <optional>
#include <utility>

// Minimal value-or-error type (the code base is built without exceptions).
namespace anchorage
{
template<typename T>
class Result
{
public:
    Result(T value)
        : m_value(std::move(value))
    {
    }
    static Result failure(QString error)
    {
        Result r;
        r.m_error = std::move(error);
        return r;
    }

    bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }
    // Reading a failed Result is a programming error: stop instead of undefined behaviour.
    T &value()
    {
        if (!m_value) {
            std::abort();
        }
        return *m_value;
    }
    const T &value() const
    {
        if (!m_value) {
            std::abort();
        }
        return *m_value;
    }
    T &operator*() { return value(); }
    const T &operator*() const { return value(); }
    T *operator->() { return &value(); }
    const T *operator->() const { return &value(); }
    const QString &error() const { return m_error; }

private:
    Result() = default;
    std::optional<T> m_value;
    QString m_error;
};

// For operations that return nothing: an empty error means success.
struct Status {
    QString error;
    bool ok() const { return error.isEmpty(); }
    explicit operator bool() const { return ok(); }
    static Status success() { return {}; }
    static Status failure(QString e) { return {std::move(e)}; }
};
} // namespace anchorage
