// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : benchmarks/fs_workload.cpp
// Purpose : Filesystem workload generator (run against a TSFS mount or a plain directory)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// Usage: tsfs_fs_workload --dir DIR [--files N] [--threads T] [--large-mib M] [--label L]
//
// Runs, inside a fresh sub-directory of DIR (removed at the end), the operations of
// the benchmark plan and prints one JSON object per line:
//   {"label":..,"op":"create_small","count":N,"seconds":S,"ops_per_s":R}
// Operations: create_small (create + 4 KiB write + close), stat (metadata lookup),
// readdir, rename, read_small, seq_write / seq_read (one large file, MiB/s, fsync
// included in the write), delete_small, and concurrent_create with T threads.
// scripts/tsfs-bench-fs.sh runs it on the backing directory and on a TSFS mount
// (GC disabled and enabled) so that the FUSE overhead can be isolated.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string dir;
    std::size_t files{10000};
    unsigned threads{4};
    std::size_t large_mib{256};
    std::string label{"run"};
};

[[noreturn]] void die(std::string_view message) {
    std::println(stderr, "tsfs_fs_workload: {}", message);
    std::exit(1);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                die(std::format("missing value for {}", a));
            }
            return argv[++i];
        };
        if (a == "--dir") {
            o.dir = value();
        } else if (a == "--files") {
            o.files = std::stoull(value());
        } else if (a == "--threads") {
            o.threads = static_cast<unsigned>(std::stoul(value()));
        } else if (a == "--large-mib") {
            o.large_mib = std::stoull(value());
        } else if (a == "--label") {
            o.label = value();
        } else if (a == "-h" || a == "--help") {
            std::println("usage: tsfs_fs_workload --dir DIR [--files N] [--threads T] [--large-mib M] [--label L]");
            std::exit(0);
        } else {
            die(std::format("unknown option {}", a));
        }
    }
    if (o.dir.empty()) {
        die("--dir is required");
    }
    if (o.files == 0 || o.threads == 0) {
        die("--files and --threads must be positive");
    }
    return o;
}

void report(const Options& o, std::string_view op, std::size_t count, double seconds,
            std::string_view unit = "ops_per_s", double amount = 0) {
    const double rate = seconds > 0 ? (amount > 0 ? amount : static_cast<double>(count)) / seconds : 0;
    std::println(R"({{"label":"{}","op":"{}","count":{},"seconds":{:.6f},"{}":{:.1f}}})", o.label, op, count, seconds,
                 unit, rate);
    (void)std::fflush(stdout);
}

template <typename F>
double timed(F&& f) {
    const auto t0 = Clock::now();
    f();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

void write_file(const std::string& path, const char* data, std::size_t size) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        die(std::format("open {}: {}", path, std::strerror(errno)));
    }
    if (::write(fd, data, size) != static_cast<ssize_t>(size)) {
        die(std::format("write {}: {}", path, std::strerror(errno)));
    }
    ::close(fd);
}

}  // namespace

static int run(int argc, char** argv) {
    const Options o = parse(argc, argv);
    const fs::path work = fs::path(o.dir) / std::format("tsfs-workload-{}", ::getpid());
    fs::create_directories(work / "small");
    const std::string small(4096, 's');
    const auto name = [&](std::size_t i) { return std::format("{}/small/f{:07}.dat", work.string(), i); };

    report(o, "create_small", o.files, timed([&] {
               for (std::size_t i = 0; i < o.files; ++i) {
                   write_file(name(i), small.data(), small.size());
               }
           }));

    report(o, "stat", o.files, timed([&] {
               struct stat st{};
               for (std::size_t i = 0; i < o.files; ++i) {
                   if (::stat(name(i).c_str(), &st) != 0) {
                       die("stat failed");
                   }
               }
           }));

    std::size_t listed = 0;
    report(o, "readdir", o.files, timed([&] {
               DIR* d = ::opendir((work / "small").c_str());
               if (d == nullptr) {
                   die("opendir failed");
               }
               while (::readdir(d) != nullptr) {
                   ++listed;
               }
               ::closedir(d);
           }));
    if (listed < o.files) {
        die(std::format("readdir listed {} of {} files", listed, o.files));
    }

    report(o, "read_small", o.files, timed([&] {
               char buffer[4096];
               for (std::size_t i = 0; i < o.files; ++i) {
                   const int fd = ::open(name(i).c_str(), O_RDONLY | O_CLOEXEC);
                   if (fd < 0 || ::read(fd, buffer, sizeof buffer) != static_cast<ssize_t>(sizeof buffer)) {
                       die("read_small failed");
                   }
                   ::close(fd);
               }
           }));

    report(o, "rename", o.files, timed([&] {
               for (std::size_t i = 0; i < o.files; ++i) {
                   if (::rename(name(i).c_str(), (name(i) + ".r").c_str()) != 0) {
                       die("rename failed");
                   }
               }
           }));

    report(o, "delete_small", o.files, timed([&] {
               for (std::size_t i = 0; i < o.files; ++i) {
                   if (::unlink((name(i) + ".r").c_str()) != 0) {
                       die("unlink failed");
                   }
               }
           }));

    // Sequential I/O on one large file, 1 MiB requests. The read follows the write, so
    // it is served from the page cache (warm-cache figure, documented as such).
    const std::string big = (work / "large.bin").string();
    const std::vector<char> chunk(1 << 20, 'L');
    const double mib = static_cast<double>(o.large_mib);
    report(o, "seq_write", o.large_mib, timed([&] {
               const int fd = ::open(big.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
               if (fd < 0) {
                   die("open large failed");
               }
               for (std::size_t i = 0; i < o.large_mib; ++i) {
                   if (::write(fd, chunk.data(), chunk.size()) != static_cast<ssize_t>(chunk.size())) {
                       die("seq_write failed");
                   }
               }
               ::fsync(fd);
               ::close(fd);
           }),
           "MiB_per_s", mib);
    report(o, "seq_read", o.large_mib, timed([&] {
               std::vector<char> buffer(1 << 20);
               const int fd = ::open(big.c_str(), O_RDONLY | O_CLOEXEC);
               if (fd < 0) {
                   die("open large failed");
               }
               while (::read(fd, buffer.data(), buffer.size()) > 0) {
               }
               ::close(fd);
           }),
           "MiB_per_s", mib);
    ::unlink(big.c_str());

    // Concurrent clients: T threads each create files in their own directory.
    const std::size_t per_thread = o.files / o.threads;
    report(o, "concurrent_create", per_thread * o.threads, timed([&] {
               std::vector<std::jthread> threads;
               threads.reserve(o.threads);
               for (unsigned t = 0; t < o.threads; ++t) {
                   threads.emplace_back([&, t] {
                       const fs::path d = work / std::format("t{}", t);
                       fs::create_directories(d);
                       for (std::size_t i = 0; i < per_thread; ++i) {
                           write_file(std::format("{}/f{}", d.string(), i), small.data(), small.size());
                       }
                   });
               }
           }));

    std::error_code ec;
    fs::remove_all(work, ec);
    return ec ? 1 : 0;
}

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape): see src/main.cpp
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {  // Invalid numbers (std::stoull), filesystem_error...
        std::println(stderr, "tsfs_fs_workload: {}", e.what());
        return 1;
    }
}
