// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/config/test_reload.cpp
// Purpose : Runtime reload: validation, preservation, observers, concurrency, inotify, SIGHUP bridge
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <thread>

#include "config/config_manager.hpp"
#include "config/config_watcher.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

class ReloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        log_.set_level(LogLevel::critical);
        write(document("1d"));
    }

    std::string document(std::string_view retention, std::string_view mountpoint = "/mnt/tsfs") const {
        return std::format(
            "version: 1\nfilesystem: {{mountpoint: {}}}\nretention:\n  rules:\n"
            "    - {{id: rec, path: /rec, retention: \"{}\"}}\nlogging: {{directory: {}}}\n",
            mountpoint, retention, (dir_ / "logs").string());
    }
    /// Atomic replacement, as editors and deployment tools do.
    void write(const std::string& content) {
        test::write_file(dir_ / "config.yml.tmp", content);
        fs::rename(dir_ / "config.yml.tmp", path_);
    }

    test::TempDir dir_;
    fs::path path_{dir_ / "config.yml"};
    Logger log_;
};

TEST_F(ReloadTest, SuccessfulReloadPublishesANewGeneration) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    const auto first = manager.current();
    EXPECT_EQ(first->generation, 1U);
    write(document("2h"));
    ASSERT_TRUE(manager.reload("test").has_value());
    const auto second = manager.current();
    EXPECT_EQ(second->generation, 2U);
    EXPECT_EQ(*second->config->retention.rules[0].retention, 2h);
    EXPECT_EQ(*first->config->retention.rules[0].retention, 24h);  // Old snapshot untouched.
    EXPECT_EQ(manager.reload_successes(), 1U);
}

TEST_F(ReloadTest, FailedReloadKeepsThePreviousConfiguration) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    for (const std::string bad : {"version: 1\nretention: [", "version: 1\nretention: {default: 1y}\n", "version: 9\n",
                                  "version: 1\nretention: {rules: [{id: x, glob: '[', retention: 1d}]}\n"}) {
        write(bad);
        EXPECT_FALSE(manager.reload("test").has_value()) << bad;
        EXPECT_EQ(manager.current()->generation, 1U);
        EXPECT_EQ(*manager.current()->config->retention.rules[0].retention, 24h);
    }
    EXPECT_EQ(manager.reload_failures(), 4U);
    fs::remove(path_);
    EXPECT_FALSE(manager.reload("test").has_value());  // Missing file.
    EXPECT_EQ(manager.current()->generation, 1U);
}

TEST_F(ReloadTest, MultipleConsecutiveReloads) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    for (int i = 1; i <= 20; ++i) {
        write(document(std::to_string(i) + "h"));
        ASSERT_TRUE(manager.reload("test").has_value());
        EXPECT_EQ(*manager.current()->config->retention.rules[0].retention, std::chrono::hours(i));
    }
    EXPECT_EQ(manager.current()->generation, 21U);
}

TEST_F(ReloadTest, RestartOnlySettingsKeepTheirRunningValue) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    write(document("3h", "/mnt/elsewhere"));
    ASSERT_TRUE(manager.reload("test").has_value());
    EXPECT_EQ(manager.current()->config->filesystem.mountpoint, "/mnt/tsfs");
    EXPECT_EQ(*manager.current()->config->retention.rules[0].retention, 3h);  // Reloadable part applied.
}

TEST_F(ReloadTest, ObserverFailureAbortsPublication) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    int calls = 0;
    manager.add_observer([&](const ConfigSnapshot&, const ConfigSnapshot&) -> Result<void> {
        ++calls;
        return make_error(EACCES, "cannot open the new log file");
    });
    write(document("5h"));
    EXPECT_FALSE(manager.reload("test").has_value());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(manager.current()->generation, 1U);
}

TEST_F(ReloadTest, CommandLineOverridesSurviveReloads) {
    ConfigOverrides overrides;
    overrides.log_level = LogLevel::error;
    ConfigManager manager(path_.string(), overrides, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    write(document("5h"));
    ASSERT_TRUE(manager.reload("test").has_value());
    EXPECT_EQ(manager.current()->config->logging.level, LogLevel::error);
}

TEST_F(ReloadTest, ConcurrentReloadsAndReaders) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    {
        std::vector<std::jthread> threads;
        for (int r = 0; r < 4; ++r) {
            threads.emplace_back([&] {
                while (!stop.load()) {
                    const auto snap = manager.current();
                    // A snapshot is always internally consistent.
                    ASSERT_EQ(snap->config->retention.rules.size(), 1U);
                    ASSERT_TRUE(snap->policy != nullptr);
                    reads.fetch_add(1);
                }
            });
        }
        std::vector<std::jthread> writers;
        for (int w = 0; w < 4; ++w) {
            writers.emplace_back([&] {
                for (int i = 0; i < 25; ++i) {
                    (void)manager.reload("test");
                }
            });
        }
        writers.clear();
        stop.store(true);
    }
    EXPECT_EQ(manager.current()->generation, 101U);
    EXPECT_GT(reads.load(), 0U);
}

TEST_F(ReloadTest, InotifyDetectsAtomicReplacementAndIgnoresNoOps) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    auto watcher =
        ConfigWatcher::create(path_.string(), {[&](std::string_view t) { (void)manager.reload(t); }, {}, {}});
    ASSERT_TRUE(watcher.has_value());
    ASSERT_TRUE((*watcher)->inotify_active());
    (*watcher)->start();
    write(document("6h"));
    EXPECT_TRUE(eventually([&] { return manager.current()->generation == 2; }));
    write(document("6h"));  // Same content: no reload.
    test::write_file(dir_ / "unrelated.txt", "noise");
    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(manager.current()->generation, 2U);
    write(document("7h"));  // Replaced again: the watch on the directory still works.
    EXPECT_TRUE(eventually([&] { return manager.current()->generation == 3; }));
    (*watcher)->stop();
}

TEST_F(ReloadTest, SighupBridgeAlwaysReloads) {
    ConfigManager manager(path_.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    std::atomic<int> status_calls{0};
    std::atomic<int> purge_calls{0};
    auto watcher =
        ConfigWatcher::create(path_.string(), {[&](std::string_view t) { (void)manager.reload(t); },
                                               [&] { status_calls.fetch_add(1); }, [&] { purge_calls.fetch_add(1); }});
    ASSERT_TRUE(watcher.has_value());
    (*watcher)->start();
    install_control_signal_handlers((*watcher)->reload_fd(), (*watcher)->status_fd(), (*watcher)->purge_fd());
    ::raise(SIGHUP);  // Unchanged file: SIGHUP still reloads.
    EXPECT_TRUE(eventually([&] { return manager.current()->generation == 2; }));
    ::raise(SIGUSR1);
    EXPECT_TRUE(eventually([&] { return status_calls.load() == 1; }));
    ::raise(SIGUSR2);  // tsfs --purge
    EXPECT_TRUE(eventually([&] { return purge_calls.load() == 1; }));
    install_control_signal_handlers(-1, -1);
    (*watcher)->stop();
}

TEST_F(ReloadTest, WatchIsReestablishedWhenTheDirectoryIsRecreated) {
    const fs::path sub = dir_ / "etc";
    fs::create_directories(sub);
    const fs::path cfg = sub / "config.yml";
    test::write_file(cfg, document("1d"));
    ConfigManager manager(cfg.string(), {}, log_);
    ASSERT_TRUE(manager.load_initial().has_value());
    auto watcher = ConfigWatcher::create(cfg.string(), {[&](std::string_view t) { (void)manager.reload(t); }, {}, {}});
    ASSERT_TRUE(watcher.has_value());
    (*watcher)->start();
    fs::remove_all(sub);
    std::this_thread::sleep_for(300ms);
    fs::create_directories(sub);
    test::write_file(cfg, document("9h"));
    EXPECT_TRUE(eventually([&] { return manager.current()->generation >= 2; }, 8s));
    EXPECT_EQ(*manager.current()->config->retention.rules[0].retention, 9h);
    (*watcher)->stop();
}

}  // namespace
}  // namespace tsfs
