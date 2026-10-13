// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/unique_fd.hpp
// Purpose : RAII owner of a POSIX file descriptor
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <unistd.h>

#include <utility>

namespace tsfs {

/**
 * @brief Exclusive owner of a file descriptor, closed on destruction.
 *
 * Move-only. A negative value means "no descriptor". Not thread-safe: one owner
 * at a time, like std::unique_ptr.
 */
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    /// Take ownership of @p fd (may be negative).
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete("a descriptor has a single owner: move it or dup() it explicitly");
    UniqueFd& operator=(const UniqueFd&) = delete("a descriptor has a single owner: move it");
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    /// @return the owned descriptor (still owned).
    [[nodiscard]] int get() const noexcept { return fd_; }
    /// @return true when a descriptor is owned.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    /// Give up ownership without closing. @return the descriptor.
    int release() noexcept { return std::exchange(fd_, -1); }

    /// Close the owned descriptor (if any) and take ownership of @p fd.
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            // close() errors are not actionable here (the descriptor is gone either way).
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

}  // namespace tsfs
