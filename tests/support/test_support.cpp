// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/support/test_support.cpp
// Purpose : Test fixtures: isolated temporary directories, file helpers, snapshot builders
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "support/test_support.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace tsfs::test {

TempDir::TempDir(std::string_view prefix) {
    std::filesystem::path base = "/tmp";
    if (const char* artifacts = std::getenv("TSFS_TEST_ARTIFACT_DIR"); artifacts != nullptr && *artifacts != '\0') {
        base = artifacts;
    } else if (const char* tmp = std::getenv("TMPDIR"); tmp != nullptr && *tmp != '\0') {
        base = tmp;
    }
    std::filesystem::create_directories(base);
    std::string pattern = (base / (std::string(prefix) + "-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        throw std::system_error(errno, std::generic_category(), "mkdtemp");
    }
    path_ = pattern;
}

TempDir::~TempDir() {
    const char* keep = std::getenv("TSFS_TEST_KEEP_ARTIFACTS");
    if (keep != nullptr && std::string_view(keep) == "1") {
        return;
    }
    std::error_code ec;
    // Make every directory writable first (tests create read-only directories).
    for (auto it = std::filesystem::recursive_directory_iterator(
             path_, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, ec);
        }
    }
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
    std::filesystem::remove_all(path_, ec);
}

void write_file(const std::filesystem::path& file, std::string_view content) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write " + file.string());
    }
    out << content;
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

void set_mtime(const std::filesystem::path& file, TimePoint when) {
    const auto ms = when.time_since_epoch().count();
    timespec times[2];
    times[0].tv_sec = ms / 1000;
    times[0].tv_nsec = (ms % 1000) * 1000000;
    times[1] = times[0];
    if (::utimensat(AT_FDCWD, file.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
        throw std::system_error(errno, std::generic_category(), "utimensat " + file.string());
    }
}

std::size_t count_lines(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::size_t n = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++n;
    }
    return n;
}

std::shared_ptr<const GcSnapshot> make_gc_snapshot(const RetentionConfig& retention, GcConfig gc,
                                                   std::uint64_t generation) {
    auto policy = PolicyEngine::compile(retention);
    if (!policy) {
        throw std::runtime_error(policy.error().message);
    }
    auto snapshot = std::make_shared<GcSnapshot>();
    snapshot->config = gc;
    snapshot->policy = *policy;
    snapshot->generation = generation;
    return snapshot;
}

RuleConfig rule(std::string id, std::optional<Duration> retention) {
    RuleConfig r;
    r.id = std::move(id);
    r.retention = retention;
    return r;
}

bool fuse_available(std::string* reason) {
    const auto fail = [&](const char* why) {
        if (reason != nullptr) {
            *reason = why;
        }
        return false;
    };
    if (std::getenv("TSFS_TEST_DISABLE_FUSE") != nullptr) {
        return fail("TSFS_TEST_DISABLE_FUSE is set");
    }
    const int fd = ::open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail("/dev/fuse cannot be opened (no FUSE support or no permission)");
    }
    ::close(fd);
    if (::access("/usr/bin/fusermount3", X_OK) != 0 && ::access("/bin/fusermount3", X_OK) != 0 && ::geteuid() != 0) {
        return fail("fusermount3 not installed");
    }
    return true;
}

}  // namespace tsfs::test
