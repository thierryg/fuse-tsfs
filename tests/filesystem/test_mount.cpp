// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/filesystem/test_mount.cpp
// Purpose : FUSE integration tests against the real tsfs binary (skipped when FUSE is unavailable)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "support/test_support.hpp"

extern char** environ;

namespace tsfs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

bool is_mounted(const fs::path& mountpoint) {
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    const std::string needle = " " + mountpoint.string() + " ";
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos && line.find("fuse.tsfs") != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// LeakSanitizer report blocks of a daemon output, split by owner.
struct LeakReport {
    std::vector<std::string> tsfs;  ///< Leaks allocated by TSFS code: defects.
    std::size_t third_party{0};     ///< Leaks allocated inside libfuse or libc.
};

/**
 * Attribute every leak block ("Direct/Indirect leak of ...") to the code that made
 * the allocation: the first frame after the allocator (#0) that is not in the C++
 * standard library headers. libfuse leaves request memory behind when a session
 * ends with requests still queued (a pending RELEASE/RELEASEDIR at SIGTERM): such
 * leaks are allocated inside libfuse3.so and are not TSFS defects. A module-wide
 * suppression would also hide TSFS allocations made from libfuse threads.
 */
LeakReport classify_leaks(const std::string& output) {
    LeakReport report;
    std::size_t pos = 0;
    while ((pos = output.find(" leak of ", pos)) != std::string::npos) {
        const std::size_t start = output.rfind('\n', pos) + 1;
        std::size_t end = output.find("\n\n", pos);
        end = end == std::string::npos ? output.size() : end;
        const std::string block = output.substr(start, end - start);
        pos = end;
        bool tsfs = false;
        std::size_t line_start = 0;
        while (line_start < block.size()) {
            const std::size_t line_end = std::min(block.find('\n', line_start), block.size());
            const std::string line = block.substr(line_start, line_end - line_start);
            line_start = line_end + 1;
            if (line.find("    #") != 0 || line.find("    #0 ") == 0) {
                continue;  // Header line or the allocator itself.
            }
            if (line.find("/usr/include/c++/") != std::string::npos) {
                continue;  // std::make_unique, allocators...: look at their caller.
            }
            tsfs =
                line.find(" in tsfs::") != std::string::npos || line.find(TSFS_SOURCE_DIR "/src/") != std::string::npos;
            break;
        }
        if (tsfs) {
            report.tsfs.push_back(block);
        } else {
            ++report.third_party;
        }
    }
    return report;
}

/// Mounts TSFS on a private temporary tree for each test and unmounts it afterwards.
class MountTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string reason;
        if (!test::fuse_available(&reason)) {
            GTEST_SKIP() << "FUSE unavailable: " << reason;
        }
        fs::create_directories(data_);
        fs::create_directories(mnt_);
        write_config(rules_);
        start();
    }

    /// Every test ends with a clean daemon shutdown: exit status 0, no memory error,
    /// no data race, no undefined behavior and no leak allocated by TSFS code (in
    /// sanitizer builds), whatever path the test exercised.
    void TearDown() override {
        const bool was_running = pid_ > 0;
        stop();
        const std::string output = test::read_file(dir_ / "tsfs.out");
        for (const char* marker : {"ERROR: AddressSanitizer", "ThreadSanitizer", "runtime error:"}) {
            EXPECT_EQ(output.find(marker), std::string::npos) << marker << " report from the daemon:\n" << output;
        }
        const LeakReport leaks = classify_leaks(output);
        for (const std::string& leak : leaks.tsfs) {
            ADD_FAILURE() << "memory leaked by TSFS code in the daemon:\n" << leak;
        }
        if (leaks.third_party > 0) {
            RecordProperty("third_party_leaks_at_exit", static_cast<int>(leaks.third_party));
        }
        if (was_running) {
            EXPECT_TRUE(WIFEXITED(exit_status_) && WEXITSTATUS(exit_status_) == 0)
                << "daemon exit status " << exit_status_ << "\n"
                << output;
        }
    }

    void write_config(const std::string& rules, std::string_view extra = "") {
        test::write_file(config_, std::format(R"(version: 1
filesystem: {{mountpoint: "{}", backing_directory: "{}"{}}}
retention:
  default: never
  rules:
{}
garbage_collector: {{scan_interval: 1s, max_deletions_per_second: 0, retry_initial_delay: 100ms}}
logging: {{directory: "{}", level: debug, async: {{enabled: false}}}}
metrics: {{status_file: "{}", interval: 1s}}
)",
                                              mnt_.string(), data_.string(), extra, rules, (dir_ / "logs").string(),
                                              (dir_ / "status.json").string()));
    }

    void start() {
        spawn();
        ASSERT_TRUE(eventually([&] { return is_mounted(mnt_); })) << test::read_file(dir_ / "tsfs.out");
    }

    /// Start the daemon without waiting for the mount.
    void spawn() {
        std::vector<std::string> args{TSFS_BINARY, "--config", config_.string(), "--foreground"};
        std::vector<char*> argv;
        for (auto& a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        const std::string out = (dir_ / "tsfs.out").string();
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        // LeakSanitizer must not turn third-party leaks at exit into a failure status:
        // TearDown() classifies the report itself (classify_leaks).
        std::vector<std::string> env_storage;
        bool lsan_set = false;
        for (char** e = environ; *e != nullptr; ++e) {
            env_storage.emplace_back(*e);
            if (env_storage.back().starts_with("LSAN_OPTIONS=")) {
                env_storage.back() += ":exitcode=0";
                lsan_set = true;
            }
        }
        if (!lsan_set) {
            env_storage.emplace_back("LSAN_OPTIONS=exitcode=0");
        }
        std::vector<char*> envp;
        envp.reserve(env_storage.size() + 1);
        for (auto& e : env_storage) {
            envp.push_back(e.data());
        }
        envp.push_back(nullptr);
        ASSERT_EQ(posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), envp.data()), 0);
        posix_spawn_file_actions_destroy(&actions);
    }

    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            exit_status_ = status;
            pid_ = -1;
        }
        if (is_mounted(mnt_)) {
            const std::string cmd = "fusermount3 -u -z '" + mnt_.string() + "'";
            [[maybe_unused]] const int rc = std::system(cmd.c_str());
        }
    }

    std::string status_json() const { return test::read_file(dir_ / "status.json"); }

    test::TempDir dir_{"tsfs-mount"};
    fs::path data_{dir_ / "data"};
    fs::path mnt_{dir_ / "mnt"};
    fs::path config_{dir_ / "config.yml"};
    std::string rules_ =
        "    - {id: rec, path: /rec, recursive: true, retention: 2s}\n"
        "    - {id: keep, action: exclude, glob: \"*.keep\"}";
    pid_t pid_{-1};
    int exit_status_{0};
};

TEST_F(MountTest, CreateReadWriteAppend) {
    test::write_file(mnt_ / "a.txt", "hello");
    EXPECT_EQ(test::read_file(mnt_ / "a.txt"), "hello");
    EXPECT_EQ(test::read_file(data_ / "a.txt"), "hello");  // Passthrough to the backing tree.
    {
        std::ofstream out(mnt_ / "a.txt", std::ios::app);
        out << " world";
    }
    EXPECT_EQ(test::read_file(mnt_ / "a.txt"), "hello world");
    test::write_file(data_ / "direct.txt", "from below");  // Visible immediately through the mount.
    EXPECT_EQ(test::read_file(mnt_ / "direct.txt"), "from below");
}

TEST_F(MountTest, TruncateAndPartialWrites) {
    test::write_file(mnt_ / "t", "0123456789");
    fs::resize_file(mnt_ / "t", 4);
    EXPECT_EQ(test::read_file(mnt_ / "t"), "0123");
    const int fd = ::open((mnt_ / "t").c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::pwrite(fd, "XY", 2, 2), 2);
    ASSERT_EQ(::ftruncate(fd, 3), 0);
    ::close(fd);
    EXPECT_EQ(test::read_file(mnt_ / "t"), "01X");
}

TEST_F(MountTest, RenameUnlinkAndDirectories) {
    fs::create_directories(mnt_ / "d1/d2");
    test::write_file(mnt_ / "d1/d2/f", "x");
    fs::rename(mnt_ / "d1/d2/f", mnt_ / "d1/g");
    EXPECT_FALSE(fs::exists(mnt_ / "d1/d2/f"));
    EXPECT_TRUE(fs::exists(data_ / "d1/g"));
    std::error_code ec;
    EXPECT_FALSE(fs::remove(mnt_ / "d1", ec));  // ENOTEMPTY.
    fs::remove(mnt_ / "d1/g");
    fs::remove(mnt_ / "d1/d2");
    fs::remove(mnt_ / "d1");
    EXPECT_FALSE(fs::exists(data_ / "d1"));
}

TEST_F(MountTest, RenameNoReplaceAndExchange) {
    test::write_file(mnt_ / "a", "A");
    test::write_file(mnt_ / "b", "B");
    EXPECT_NE(::renameat2(AT_FDCWD, (mnt_ / "a").c_str(), AT_FDCWD, (mnt_ / "b").c_str(), RENAME_NOREPLACE), 0);
    EXPECT_EQ(errno, EEXIST);
    ASSERT_EQ(::renameat2(AT_FDCWD, (mnt_ / "a").c_str(), AT_FDCWD, (mnt_ / "b").c_str(), RENAME_EXCHANGE), 0);
    EXPECT_EQ(test::read_file(mnt_ / "a"), "B");
    EXPECT_EQ(test::read_file(mnt_ / "b"), "A");
}

TEST_F(MountTest, ReaddirOfALargeDirectory) {
    for (int i = 0; i < 1500; ++i) {
        test::write_file(data_ / "big" / ("file-" + std::to_string(i)), "");
    }
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& _ : fs::directory_iterator(mnt_ / "big")) {
        ++count;
    }
    EXPECT_EQ(count, 1500U);
}

TEST_F(MountTest, PermissionsOwnershipAndTimestamps) {
    test::write_file(mnt_ / "p", "x");
    fs::permissions(mnt_ / "p", fs::perms::owner_read | fs::perms::owner_write);
    struct stat st{};
    ASSERT_EQ(::stat((mnt_ / "p").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600U);
    EXPECT_EQ(st.st_uid, ::getuid());
    struct stat backing{};
    ASSERT_EQ(::stat((data_ / "p").c_str(), &backing), 0);
    EXPECT_EQ(st.st_ino, backing.st_ino);  // use_ino: backing inode numbers.
    test::set_mtime(mnt_ / "p", test::kEpoch2026);
    ASSERT_EQ(::stat((data_ / "p").c_str(), &backing), 0);
    EXPECT_EQ(backing.st_mtim.tv_sec, test::kEpoch2026.time_since_epoch().count() / 1000);
    if (::geteuid() != 0) {
        fs::permissions(mnt_ / "p", fs::perms::none);
        EXPECT_FALSE(std::ifstream(mnt_ / "p").is_open());  // default_permissions enforced.
        fs::permissions(mnt_ / "p", fs::perms::owner_all);
    }
}

TEST_F(MountTest, SymlinksAndHardLinks) {
    test::write_file(mnt_ / "target", "T");
    fs::create_symlink("target", mnt_ / "rel-link");
    fs::create_symlink("/etc/hostname", mnt_ / "abs-link");
    EXPECT_EQ(fs::read_symlink(mnt_ / "rel-link"), "target");
    EXPECT_EQ(fs::read_symlink(data_ / "abs-link"), "/etc/hostname");  // Stored verbatim.
    EXPECT_EQ(test::read_file(mnt_ / "rel-link"), "T");
    fs::create_hard_link(mnt_ / "target", mnt_ / "hard");
    EXPECT_EQ(fs::hard_link_count(mnt_ / "target"), 2U);
    EXPECT_TRUE(fs::equivalent(data_ / "target", data_ / "hard"));
}

TEST_F(MountTest, DeviceNodesAreRefused) {
    EXPECT_NE(::mknod((mnt_ / "dev").c_str(), S_IFCHR | 0600, makedev(1, 3)), 0);
    EXPECT_EQ(errno, EPERM);
    EXPECT_EQ(::mkfifo((mnt_ / "fifo").c_str(), 0600), 0);
    EXPECT_TRUE(fs::is_fifo(data_ / "fifo"));
}

TEST_F(MountTest, StatfsReportsTheBackingFilesystem) {
    struct statvfs a{};
    struct statvfs b{};
    ASSERT_EQ(::statvfs(mnt_.c_str(), &a), 0);
    ASSERT_EQ(::statvfs(data_.c_str(), &b), 0);
    EXPECT_EQ(a.f_blocks, b.f_blocks);
    EXPECT_EQ(a.f_bsize, b.f_bsize);
}

TEST_F(MountTest, LargeFileRoundTrip) {
    std::string block(1 << 20, '\0');
    std::mt19937 rng(7);
    for (auto& c : block) {
        c = static_cast<char>(rng());
    }
    {
        std::ofstream out(mnt_ / "large.bin", std::ios::binary);
        for (int i = 0; i < 64; ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    EXPECT_EQ(fs::file_size(mnt_ / "large.bin"), 64U << 20);
    std::ifstream in(mnt_ / "large.bin", std::ios::binary);
    std::string chunk(block.size(), '\0');
    for (int i = 0; i < 64; ++i) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        ASSERT_EQ(chunk, block) << "block " << i;
    }
}

TEST_F(MountTest, ConcurrentClients) {
    std::atomic<int> errors{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                const fs::path d = mnt_ / ("t" + std::to_string(t));
                std::error_code ec;
                fs::create_directories(d, ec);
                for (int i = 0; i < 100; ++i) {
                    const fs::path f = d / ("f" + std::to_string(i));
                    test::write_file(f, std::to_string(i));
                    if (test::read_file(f) != std::to_string(i)) {
                        ++errors;
                    }
                    fs::rename(f, d / ("g" + std::to_string(i)), ec);
                    if (ec || !fs::remove(d / ("g" + std::to_string(i)), ec)) {
                        ++errors;
                    }
                }
            });
        }
    }
    EXPECT_EQ(errors.load(), 0);
}

TEST_F(MountTest, GarbageCollectorDeletesExpiredFilesThroughTheMount) {
    fs::create_directories(mnt_ / "rec");
    test::write_file(mnt_ / "rec/video.mp4", "v");
    test::write_file(mnt_ / "rec/important.keep", "k");
    test::write_file(mnt_ / "elsewhere.mp4", "e");
    EXPECT_TRUE(eventually([&] { return !fs::exists(mnt_ / "rec/video.mp4"); }));
    EXPECT_FALSE(fs::exists(data_ / "rec/video.mp4"));
    EXPECT_TRUE(fs::exists(mnt_ / "rec/important.keep"));
    EXPECT_TRUE(fs::exists(mnt_ / "elsewhere.mp4"));
    EXPECT_TRUE(eventually([&] { return status_json().find("\"deleted\": 1,") != std::string::npos; }));
}

TEST_F(MountTest, OpenFileIsNotDeletedUntilReleased) {
    fs::create_directories(mnt_ / "rec");
    const int fd = ::open((mnt_ / "rec/open.bin").c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, "x", 1), 1);
    std::this_thread::sleep_for(4s);  // Well past the 2 s retention.
    EXPECT_TRUE(fs::exists(data_ / "rec/open.bin"));
    ::close(fd);
    EXPECT_TRUE(eventually([&] { return !fs::exists(data_ / "rec/open.bin"); }));
}

TEST_F(MountTest, ReloadBySighupWithoutRemounting) {
    test::write_file(mnt_ / "logs/app.log", "l");
    test::set_mtime(mnt_ / "logs/app.log", test::kEpoch2026);
    std::this_thread::sleep_for(1500ms);
    EXPECT_TRUE(fs::exists(mnt_ / "logs/app.log"));  // No rule yet.
    write_config(rules_ + "\n    - {id: logs, path: /logs, retention: 1h}");
    ::kill(pid_, SIGHUP);
    EXPECT_TRUE(eventually([&] { return !fs::exists(mnt_ / "logs/app.log"); }));
    EXPECT_TRUE(is_mounted(mnt_));
    // The status file is refreshed every metrics.interval (1 s).
    EXPECT_TRUE(eventually([&] { return status_json().find("\"generation\": 2") != std::string::npos; }));
}

TEST_F(MountTest, InvalidReloadKeepsServing) {
    test::write_file(config_, "version: 1\nretention: [broken\n");
    ::kill(pid_, SIGHUP);
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(is_mounted(mnt_));
    test::write_file(mnt_ / "still", "works");
    EXPECT_EQ(test::read_file(mnt_ / "still"), "works");
    EXPECT_NE(test::read_file(dir_ / "logs/tsfs.log").find("event=config_reload_failed"), std::string::npos);
}

/// stay_on_filesystem (default): a file bind-mounted from another filesystem onto a
/// name inside the backing tree is outside the managed tree: neither its attributes
/// nor its content are reachable through the mount (EXDEV). Needs root (mount(2)).
TEST_F(MountTest, FileBindMountedFromAnotherFilesystemIsNotReachable) {
    if (::geteuid() != 0) {
        GTEST_SKIP() << "needs root to create a bind mount";
    }
    const fs::path other = dir_ / "other-fs";
    fs::create_directories(other);
    ASSERT_EQ(::mount("tsfs-test", other.c_str(), "tmpfs", 0, "size=1m"), 0) << std::strerror(errno);
    test::write_file(other / "secret", "outside");
    test::write_file(data_ / "bound", "placeholder");
    ASSERT_EQ(::mount((other / "secret").c_str(), (data_ / "bound").c_str(), nullptr, MS_BIND, nullptr), 0)
        << std::strerror(errno);
    struct stat st{};
    EXPECT_NE(::stat((mnt_ / "bound").c_str(), &st), 0);
    EXPECT_EQ(errno, EXDEV);
    EXPECT_LT(::open((mnt_ / "bound").c_str(), O_RDONLY), 0);
    EXPECT_NE(::truncate((mnt_ / "bound").c_str(), 0), 0);
    EXPECT_EQ(test::read_file(other / "secret"), "outside");
    EXPECT_EQ(::umount2((data_ / "bound").c_str(), MNT_DETACH), 0);
    EXPECT_EQ(::umount2(other.c_str(), MNT_DETACH), 0);
}

/// The mode of a new entry is the one requested by the client after the client's
/// umask only: the daemon's umask (inherited here: 022) is not applied again.
TEST_F(MountTest, NewEntryModeIgnoresTheDaemonUmask) {
    const mode_t previous = ::umask(0);
    const int fd = ::open((mnt_ / "shared").c_str(), O_CREAT | O_WRONLY | O_EXCL, 0666);
    const int mkdir_rc = ::mkdir((mnt_ / "shared-dir").c_str(), 0777);
    ::umask(previous);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    ::close(fd);
    ASSERT_EQ(mkdir_rc, 0) << std::strerror(errno);
    struct stat st{};
    ASSERT_EQ(::stat((data_ / "shared").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0666U);
    ASSERT_EQ(::stat((data_ / "shared-dir").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0777U);
}

/// Regression: handles still open when the daemon stops (no RELEASE will ever come)
/// are freed by the filesystem; in sanitizer builds a leak fails TearDown.
TEST_F(MountTest, ShutdownWithOpenFilesFreesTheirHandles) {
    const int file = ::open((mnt_ / "still-open").c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(file, 0) << std::strerror(errno);
    ASSERT_EQ(::write(file, "x", 1), 1);
    const int dir = ::open(mnt_.c_str(), O_RDONLY | O_DIRECTORY);
    ASSERT_GE(dir, 0) << std::strerror(errno);
    ::kill(pid_, SIGTERM);  // Stop while both are open.
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    exit_status_ = status;
    ::close(file);
    ::close(dir);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status << "\n" << test::read_file(dir_ / "tsfs.out");
    const std::string output = test::read_file(dir_ / "tsfs.out");
    EXPECT_TRUE(classify_leaks(output).tsfs.empty()) << output;
    const std::string log = test::read_file(dir_ / "logs/tsfs.log");
    EXPECT_NE(log.find("event=filesystem_handles_released files=1 directories=1"), std::string::npos) << log;
}

/// A second instance configured with a mount point that is already mounted (here by
/// the fixture's daemon) is refused before mounting: no stacked mount.
TEST_F(MountTest, SecondInstanceOnAMountedMountPointIsRefused) {
    const fs::path other = dir_ / "second.yml";
    test::write_file(other, std::format(R"(version: 1
filesystem: {{mountpoint: "{}", backing_directory: "{}"}}
logging: {{directory: "{}"}}
metrics: {{status_file: "{}"}}
)",
                                        mnt_.string(), (dir_ / "data2").string(), (dir_ / "logs2").string(),
                                        (dir_ / "status2.json").string()));
    fs::create_directories(dir_ / "data2");
    const std::string out = (dir_ / "second.out").string();
    const std::string cmd =
        std::format("'{}' --config '{}' --foreground > '{}' 2>&1", TSFS_BINARY, other.string(), out);
    const int status = std::system(cmd.c_str());
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 7) << test::read_file(out);
    EXPECT_NE(test::read_file(out).find("is already a mount point"), std::string::npos) << test::read_file(out);
    EXPECT_TRUE(is_mounted(mnt_));  // The first instance keeps serving.
    test::write_file(mnt_ / "still-served", "ok");
    EXPECT_EQ(test::read_file(data_ / "still-served"), "ok");
}

/// The status file names the kernel FUSE connection of the mount (fusectl).
TEST_F(MountTest, StatusFileNamesTheFuseConnection) {
    struct stat st{};
    ASSERT_EQ(::stat(mnt_.c_str(), &st), 0);
    const auto expected = (major(st.st_dev) << 20U) | minor(st.st_dev);
    ASSERT_TRUE(eventually([&] { return status_json().find("\"fuse\": {") != std::string::npos; }));
    const std::string status = status_json();
    EXPECT_NE(status.find(std::format("\"connection_id\": {}, \"control_dir\": \"/sys/fs/fuse/connections/{}\"",
                                      expected, expected)),
              std::string::npos)
        << status;
    // waiting is a number where fusectl is mounted (hosts), null otherwise (containers).
    const bool fusectl = fs::exists(std::format("/sys/fs/fuse/connections/{}/waiting", expected));
    EXPECT_EQ(status.find("\"waiting\": null") == std::string::npos, fusectl) << status;
}

/// Regression: a signal received during startup (before the handlers exist) must not
/// kill the daemon with the default action and leave a dead mount (ENOTCONN): the
/// signals are blocked from the start, so SIGTERM ends in a clean unmount, exit 0.
TEST_F(MountTest, SigtermDuringStartupUnmountsCleanly) {
    stop();
    for (int i = 0; i < 10; ++i) {
        std::filesystem::remove(dir_ / "tsfs.out");
        spawn();
        // As early as possible: once the daemon announces the mount.
        ASSERT_TRUE(eventually(
            [&] { return test::read_file(dir_ / "tsfs.out").find("filesystem_mount_started") != std::string::npos; }));
        ::kill(pid_, SIGTERM);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "iteration " << i << ", status " << status << "\n"
                                                                   << test::read_file(dir_ / "tsfs.out");
        EXPECT_FALSE(is_mounted(mnt_)) << "iteration " << i << ": mount left behind";
        if (is_mounted(mnt_)) {
            const std::string cmd = "fusermount3 -u -z '" + mnt_.string() + "'";
            [[maybe_unused]] const int rc = std::system(cmd.c_str());
        }
    }
}

/// Read-only retention attributes: what the GC will do with each file.
TEST_F(MountTest, RetentionExtendedAttributes) {
    const auto get = [](const fs::path& p, const char* name) -> std::string {
        std::array<char, 256> buffer{};
        const ssize_t n = ::getxattr(p.c_str(), name, buffer.data(), buffer.size());
        return n < 0 ? "errno:" + std::to_string(errno) : std::string(buffer.data(), static_cast<std::size_t>(n));
    };
    test::write_file(mnt_ / "rec/clip.mp4", "x");   // rule rec: 2s
    test::write_file(mnt_ / "rec/hold.keep", "x");  // exclude rule keep
    test::write_file(mnt_ / "other.txt", "x");      // default: never
    EXPECT_EQ(get(mnt_ / "rec/clip.mp4", "user.tsfs.state"), "expires");
    EXPECT_EQ(get(mnt_ / "rec/clip.mp4", "user.tsfs.rule"), "rec");
    EXPECT_EQ(get(mnt_ / "rec/clip.mp4", "user.tsfs.created").size(), 20U);  // Birth time (or mtime), UTC.
    const std::string expires = get(mnt_ / "rec/clip.mp4", "user.tsfs.expires");
    EXPECT_EQ(expires.size(), 20U) << expires;  // 2026-10-13T12:00:00Z
    EXPECT_EQ(expires.back(), 'Z') << expires;
    EXPECT_EQ(get(mnt_ / "rec/hold.keep", "user.tsfs.state"), "excluded");
    EXPECT_EQ(get(mnt_ / "rec/hold.keep", "user.tsfs.rule"), "keep");
    EXPECT_EQ(get(mnt_ / "rec/hold.keep", "user.tsfs.expires"), "errno:" + std::to_string(ENODATA));
    EXPECT_EQ(get(mnt_ / "other.txt", "user.tsfs.state"), "never");
    EXPECT_EQ(get(mnt_ / "other.txt", "user.tsfs.rule"), "default");
    EXPECT_EQ(get(mnt_ / "rec", "user.tsfs.state"), "errno:" + std::to_string(ENODATA));  // Directories: none.
    EXPECT_EQ(get(mnt_ / "other.txt", "user.other"), "errno:" + std::to_string(ENODATA));
    EXPECT_NE(::setxattr((mnt_ / "other.txt").c_str(), "user.tsfs.state", "never", 5, 0), 0);  // Read-only.
    // Not listed (GNU ls would print a "+" after the mode of every file): read by name only.
    std::array<char, 256> list{};
    EXPECT_EQ(::listxattr((mnt_ / "rec/clip.mp4").c_str(), list.data(), list.size()), 0);
}

/// tsfs --purge --force asks the running daemon (SIGUSR2) to delete every expired file
/// now, whatever the rate limit; tsfs --stats counts the files per rule.
TEST_F(MountTest, PurgeCommandDeletesExpiredFilesThroughTheDaemon) {
    stop();
    std::string config = test::read_file(config_);
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"scan_interval: 1s", "scan_interval: 1h"},
                                   {"max_deletions_per_second: 0", "max_deletions_per_second: 1"}}) {
        ASSERT_NE(config.find(from), std::string::npos);
        config.replace(config.find(from), from.size(), to);
    }
    test::write_file(config_, config);
    for (int i = 0; i < 20; ++i) {
        test::write_file(data_ / "rec" / std::format("old-{}", i), "x");
        test::set_mtime(data_ / "rec" / std::format("old-{}", i), test::kEpoch2026);  // Long expired.
    }
    test::write_file(data_ / "rec/hold.keep", "x");
    test::set_mtime(data_ / "rec/hold.keep", test::kEpoch2026);
    start();
    const auto run = [&](std::string_view args) {
        const std::string out = (dir_ / "cli.out").string();
        const int status = std::system(
            std::format("'{}' {} -c '{}' > '{}' 2>&1 < /dev/null", TSFS_BINARY, args, config_.string(), out).c_str());
        return std::pair{WIFEXITED(status) ? WEXITSTATUS(status) : -1, test::read_file(out)};
    };
    const auto [stats_rc, stats] = run("--stats");
    EXPECT_EQ(stats_rc, 0) << stats;
    EXPECT_NE(stats.find("rec"), std::string::npos) << stats;
    const auto [purge_rc, purge] = run("--purge --force");
    EXPECT_EQ(purge_rc, 0) << purge;
    EXPECT_NE(purge.find("file(s) deleted"), std::string::npos) << purge;
    std::size_t left = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(data_ / "rec")) {
        ++left;
    }
    EXPECT_EQ(left, 1U);  // Only the excluded file.
    EXPECT_TRUE(fs::exists(data_ / "rec/hold.keep"));
    EXPECT_NE(test::read_file(dir_ / "logs/tsfs.log").find("event=gc_purge_completed"), std::string::npos);
}

/// tsfs-ls: on a terminal inside a TSFS mount, the retention of each file is shown;
/// in a pipe, the output is exactly the original ls (scripts are unaffected).
TEST_F(MountTest, TsfsLsShowsRetentionOnATerminalAndDelegatesOtherwise) {
    if (std::system("command -v script > /dev/null 2>&1") != 0 || !fs::exists("/usr/bin/ls")) {
        GTEST_SKIP() << "needs script(1) (util-linux) and /usr/bin/ls";
    }
    test::write_file(mnt_ / "rec/clip.mp4", "x");
    test::write_file(mnt_ / "rec/hold.keep", "x");
    const std::string out = (dir_ / "ls.out").string();
    const auto shell = [&](const std::string& command) {
        const int rc = std::system(std::format("TSFS_REAL_LS=/usr/bin/ls {} > '{}' 2>&1", command, out).c_str());
        EXPECT_EQ(rc, 0) << command << "\n" << test::read_file(out);
        return test::read_file(out);
    };
    // script(1) gives tsfs-ls a terminal.
    const std::string tty =
        shell(std::format("script -qec \"'{}' -l '{}'\" /dev/null", TSFS_LS_BINARY, (mnt_ / "rec").string()));
    EXPECT_NE(tty.find("clip.mp4  created "), std::string::npos) << tty;
    EXPECT_NE(tty.find(", filter rec, deleted "), std::string::npos) << tty;
    EXPECT_NE(tty.find("hold.keep  UNFILTERED"), std::string::npos) << tty;
    // Options are those of the system ls (here: -R and an ignore pattern).
    const std::string recursive =
        shell(std::format("script -qec \"'{}' -R -I '*.keep' '{}'\" /dev/null", TSFS_LS_BINARY, mnt_.string()));
    EXPECT_EQ(recursive.find("hold.keep"), std::string::npos) << recursive;
    EXPECT_NE(recursive.find("clip.mp4  created "), std::string::npos) << recursive;
    const std::string piped = shell(std::format("'{}' -la '{}'", TSFS_LS_BINARY, (mnt_ / "rec").string()));
    const std::string real = shell(std::format("/usr/bin/ls -la '{}'", (mnt_ / "rec").string()));
    EXPECT_EQ(piped, real);
}

TEST_F(MountTest, CleanShutdownUnmountsAndExitsZero) {
    stop();
    EXPECT_FALSE(is_mounted(mnt_));
    ASSERT_TRUE(WIFEXITED(exit_status_)) << test::read_file(dir_ / "tsfs.out");
    EXPECT_EQ(WEXITSTATUS(exit_status_), 0) << test::read_file(dir_ / "tsfs.out");
    const std::string log = test::read_file(dir_ / "logs/tsfs.log");
    EXPECT_NE(log.find("event=filesystem_unmounted"), std::string::npos);
    EXPECT_NE(log.find("event=gc_completed"), std::string::npos);
}

class ReadOnlyMountTest : public MountTest {
protected:
    void SetUp() override {
        std::string reason;
        if (!test::fuse_available(&reason)) {
            GTEST_SKIP() << "FUSE unavailable: " << reason;
        }
        fs::create_directories(data_);
        fs::create_directories(mnt_);
        test::write_file(data_ / "existing", "x");
        write_config(rules_, ", read_only: true");
        start();
    }
};

TEST_F(ReadOnlyMountTest, WritesAreRefused) {
    EXPECT_EQ(test::read_file(mnt_ / "existing"), "x");
    EXPECT_LT(::open((mnt_ / "new").c_str(), O_CREAT | O_WRONLY, 0644), 0);
    EXPECT_EQ(errno, EROFS);
    EXPECT_NE(::unlink((mnt_ / "existing").c_str()), 0);
}

}  // namespace
}  // namespace tsfs
