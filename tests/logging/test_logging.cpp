// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/logging/test_logging.cpp
// Purpose : Logger, size-based rotation, compression, reconfiguration and failure handling
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>

#include "logging/log_compressor.hpp"
#include "logging/logger.hpp"
#include "logging/rotating_file_sink.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class LoggingTest : public ::testing::Test {
protected:
    LoggingConfig config(bool async = false) {
        LoggingConfig c;
        c.directory = (dir_ / "logs").string();
        c.level = LogLevel::trace;
        c.async.enabled = async;
        c.rotation.max_file_size = 4096;
        c.rotation.max_files = 3;
        c.rotation.check_interval = 100ms;
        return c;
    }
    fs::path active() const { return dir_ / "logs" / "tsfs.log"; }
    fs::path rotated(int i, bool gz = false) const {
        return dir_ / "logs" / ("tsfs.log." + std::to_string(i) + (gz ? ".gz" : ""));
    }
    std::size_t total_lines() const {
        std::size_t n = test::count_lines(active());
        for (int i = 1; i <= 20; ++i) {
            n += test::count_lines(rotated(i));
        }
        return n;
    }

    test::TempDir dir_;
    Logger log_;
};

TEST_F(LoggingTest, CreatesDirectoryAndActiveFile) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    log_.log(LogLevel::info, "test", "test_event", "hello", {{"path", "/a b"}});
    log_.flush();
    EXPECT_TRUE(fs::is_directory(dir_ / "logs"));
    const std::string content = test::read_file(active());
    EXPECT_NE(content.find("component=test event=test_event path=\"/a b\""), std::string::npos) << content;
    EXPECT_NE(content.find(" INFO pid="), std::string::npos);
    const auto perms = fs::status(active()).permissions();
    EXPECT_EQ(perms & fs::perms::others_write, fs::perms::none);  // Never world-writable.
}

TEST_F(LoggingTest, LineFormatHasMillisecondsAndOffset) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    log_.log(LogLevel::warning, "test", "e", "m");
    const std::string content = test::read_file(active());
    const std::regex line(
        R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}[+-]\d\d:\d\d WARNING pid=\d+ tid=\d+ component=test event=e)");
    EXPECT_TRUE(std::regex_search(content, line)) << content;
}

TEST_F(LoggingTest, EveryLevelAndRuntimeFiltering) {
    auto c = config();
    c.level = LogLevel::warning;
    ASSERT_TRUE(log_.configure(c).has_value());
    for (const LogLevel l :
         {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warning, LogLevel::error, LogLevel::critical}) {
        log_.log(l, "test", "level", to_string(l));
    }
    EXPECT_EQ(test::count_lines(active()), 3U);
    log_.set_level(LogLevel::trace);
    log_.log(LogLevel::trace, "test", "level", "trace now");
    EXPECT_EQ(test::count_lines(active()), 4U);
    const std::string content = test::read_file(active());
    for (const char* name : {"WARNING", "ERROR", "CRITICAL", "TRACE"}) {
        EXPECT_NE(content.find(name), std::string::npos) << name;
    }
}

TEST_F(LoggingTest, JsonLinesFormat) {
    auto c = config();
    c.format = LogFormat::json;
    ASSERT_TRUE(log_.configure(c).has_value());
    log_.log(LogLevel::info, "gc", "gc_file_deleted", "", {{"path", "/x\"y\n"}, {"bytes", 42}});
    const std::string content = test::read_file(active());
    EXPECT_EQ(content.front(), '{');
    EXPECT_NE(content.find(R"("level":"info")"), std::string::npos) << content;
    EXPECT_NE(content.find(R"("path":"/x\"y\n","bytes":42)"), std::string::npos) << content;
    EXPECT_EQ(content.substr(content.size() - 2), "}\n");
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 1);  // The newline in the path was escaped.
}

TEST_F(LoggingTest, RotatesAtThresholdAndKeepsMaxFiles) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    const std::string payload(200, 'x');
    for (int i = 0; i < 200; ++i) {
        log_.log(LogLevel::info, "test", "fill", payload, {{"i", i}});
    }
    EXPECT_TRUE(fs::exists(rotated(1)));
    EXPECT_TRUE(fs::exists(rotated(2)));
    EXPECT_TRUE(fs::exists(rotated(3)));
    EXPECT_FALSE(fs::exists(rotated(4)));  // max_files = 3, active file not counted.
    for (const auto& f : {active(), rotated(1), rotated(2), rotated(3)}) {
        EXPECT_LE(fs::file_size(f), 4096U) << f;  // Strict bound (no single message exceeds it).
    }
    EXPECT_GT(log_.stats().rotations, 3U);
    // The newest messages are in the active file, the previous ones in .1.
    EXPECT_NE(test::read_file(active()).find("i=199"), std::string::npos);
}

TEST_F(LoggingTest, RotationDisabledGrowsOneFile) {
    auto c = config();
    c.rotation.enabled = false;
    ASSERT_TRUE(log_.configure(c).has_value());
    const std::string payload(200, 'x');
    for (int i = 0; i < 100; ++i) {
        log_.log(LogLevel::info, "test", "fill", payload);
    }
    EXPECT_FALSE(fs::exists(rotated(1)));
    EXPECT_GT(fs::file_size(active()), 4096U * 4);
}

TEST_F(LoggingTest, LoggingDisabledCreatesNoFile) {
    auto c = config();
    c.enabled = false;
    ASSERT_TRUE(log_.configure(c).has_value());
    log_.log(LogLevel::info, "test", "e", "not written to a file");
    EXPECT_FALSE(fs::exists(active()));
}

TEST_F(LoggingTest, ConcurrentWritersWithRotationLoseNothingWithBlockPolicy) {
    auto c = config(/*async=*/true);
    c.async.queue_size = 256;
    c.rotation.max_file_size = 64 * 1024;
    c.rotation.max_files = 50;
    ASSERT_TRUE(log_.configure(c).has_value());
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    log_.log(LogLevel::info, "test", "concurrent", "", {{"t", t}, {"i", i}});
                }
            });
        }
    }
    log_.shutdown();  // Drains the queue (and logs the rotation notices).
    std::size_t lines = 0;
    for (int i = 0; i <= 50; ++i) {
        std::ifstream in(i == 0 ? active() : rotated(i));
        std::string line;
        while (std::getline(in, line)) {
            // Every line is complete: either a test message or a rotation notice.
            ASSERT_TRUE(line.starts_with("20")) << line;  // Starts with its own timestamp: not torn.
            const bool ours = line.find("event=concurrent t=") != std::string::npos;
            ASSERT_TRUE(ours || line.find("component=logging event=log_rotation") != std::string::npos) << line;
            lines += ours ? 1 : 0;
        }
    }
    EXPECT_EQ(lines, static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(log_.stats().queue_overruns, 0U);
}

TEST_F(LoggingTest, DiscardNewPolicyCountsAndReportsLosses) {
    auto c = config(/*async=*/true);
    c.async.queue_size = 64;
    c.async.overflow_policy = LogOverflowPolicy::discard_new;
    c.rotation.max_file_size = 1ULL << 30;
    ASSERT_TRUE(log_.configure(c).has_value());
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 20000; ++i) {
                    log_.log(LogLevel::info, "test", "flood", "");
                }
            });
        }
    }
    log_.flush();
    std::this_thread::sleep_for(100ms);
    const auto dropped = log_.stats().queue_overruns;
    EXPECT_GT(dropped, 0U);
    log_.maintenance();
    log_.shutdown();
    EXPECT_NE(test::read_file(active()).find("event=log_queue_overflow"), std::string::npos);
    EXPECT_EQ(test::count_lines(active()) + dropped, 160000U + 1U);
}

TEST_F(LoggingTest, OverrunOldestPolicyIsAccepted) {
    auto c = config(/*async=*/true);
    c.async.overflow_policy = LogOverflowPolicy::overrun_oldest;
    ASSERT_TRUE(log_.configure(c).has_value());
    log_.log(LogLevel::info, "test", "e", "x");
    log_.shutdown();
    EXPECT_EQ(test::count_lines(active()), 1U);
}

TEST_F(LoggingTest, ShutdownFlushesQueuedMessages) {
    auto c = config(/*async=*/true);
    c.async.queue_size = 100000;
    c.rotation.max_file_size = 1ULL << 30;
    ASSERT_TRUE(log_.configure(c).has_value());
    for (int i = 0; i < 20000; ++i) {
        log_.log(LogLevel::info, "test", "queued", "");
    }
    log_.shutdown();
    EXPECT_EQ(test::count_lines(active()), 20000U);
}

TEST_F(LoggingTest, ReconfigureToNewDirectoryAndFilename) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    log_.log(LogLevel::info, "test", "before", "");
    auto c = config();
    c.directory = (dir_ / "other").string();
    c.filename = "renamed.log";
    ASSERT_TRUE(log_.configure(c).has_value());
    log_.log(LogLevel::info, "test", "after", "");
    EXPECT_NE(test::read_file(active()).find("event=before"), std::string::npos);
    EXPECT_EQ(test::read_file(active()).find("event=after"), std::string::npos);
    EXPECT_NE(test::read_file(dir_ / "other/renamed.log").find("event=after"), std::string::npos);
    EXPECT_EQ(log_.stats().reconfigurations, 2U);
}

TEST_F(LoggingTest, FailedReconfigurationKeepsThePreviousLogger) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    test::write_file(dir_ / "blocker", "a file where a directory is expected");
    auto bad = config();
    bad.directory = (dir_ / "blocker" / "logs").string();
    const auto r = log_.configure(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ENOTDIR);
    log_.log(LogLevel::info, "test", "still_here", "");
    EXPECT_NE(test::read_file(active()).find("event=still_here"), std::string::npos);
    EXPECT_EQ(log_.active_config().directory, config().directory);
}

TEST_F(LoggingTest, PermissionFailureIsReported) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permissions";
    }
    fs::create_directories(dir_ / "ro");
    fs::permissions(dir_ / "ro", fs::perms::owner_read | fs::perms::owner_exec);
    auto c = config();
    c.directory = (dir_ / "ro" / "logs").string();
    const auto r = log_.configure(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, EACCES);
    EXPECT_FALSE(RotatingFileSink::check_writable(c.directory, c.filename).has_value());
}

TEST_F(LoggingTest, ExternallyDeletedActiveFileIsRecreated) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    log_.log(LogLevel::info, "test", "first", "");
    fs::remove(active());
    std::this_thread::sleep_for(150ms);  // > check_interval
    log_.log(LogLevel::info, "test", "second", "");
    ASSERT_TRUE(fs::exists(active()));
    EXPECT_NE(test::read_file(active()).find("event=second"), std::string::npos);
    log_.maintenance();
    EXPECT_NE(test::read_file(active()).find("event=log_file_reopened"), std::string::npos);
}

TEST_F(LoggingTest, ExternallyReplacedActiveFileIsReopened) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    log_.log(LogLevel::info, "test", "first", "");
    fs::rename(active(), dir_ / "logs" / "moved.log");  // e.g. an external logrotate
    std::this_thread::sleep_for(150ms);
    log_.log(LogLevel::info, "test", "second", "");
    EXPECT_NE(test::read_file(active()).find("event=second"), std::string::npos);
    EXPECT_EQ(test::read_file(dir_ / "logs" / "moved.log").find("event=second"), std::string::npos);
}

std::string gunzip(const fs::path& path) {
    gzFile in = gzopen(path.c_str(), "rb");
    std::string out;
    char buffer[4096];
    int n = 0;
    while ((n = gzread(in, buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
    }
    gzclose(in);
    return out;
}

TEST_F(LoggingTest, CompressionReplacesRotatedFilesAfterVerification) {
    auto c = config();
    c.rotation.compress = true;
    c.rotation.compression_level = 9;
    ASSERT_TRUE(log_.configure(c).has_value());
    const std::string payload(300, 'z');
    for (int i = 0; i < 20; ++i) {
        log_.log(LogLevel::info, "test", "fill", payload, {{"i", i}});
    }
    log_.shutdown();  // Waits for the compressor.
    ASSERT_TRUE(fs::exists(rotated(1, true)));
    EXPECT_FALSE(fs::exists(rotated(1)));
    const std::string text = gunzip(rotated(1, true));
    EXPECT_NE(text.find("event=fill"), std::string::npos);
    EXPECT_LT(fs::file_size(rotated(1, true)), text.size());
    for (const auto& entry : fs::directory_iterator(dir_ / "logs")) {
        EXPECT_FALSE(entry.path().filename().string().ends_with(".tmp")) << entry.path();
    }
}

TEST_F(LoggingTest, CompressionFailureKeepsTheOriginal) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permissions";
    }
    const fs::path logs = dir_ / "logs";
    fs::create_directories(logs);
    test::write_file(logs / "tsfs.log.1", "precious log data\n");
    struct stat st{};
    ASSERT_EQ(::stat((logs / "tsfs.log.1").c_str(), &st), 0);
    fs::permissions(logs, fs::perms::owner_read | fs::perms::owner_exec);  // No temp file possible.
    auto notices = std::make_shared<NoticeQueue>();
    LogCompressor compressor(notices);
    auto dirfd = std::make_shared<UniqueFd>(::open(logs.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    EXPECT_FALSE(compressor.run_job({dirfd, "tsfs.log", st.st_ino, 6, std::make_shared<std::mutex>()}));
    fs::permissions(logs, fs::perms::owner_all);
    EXPECT_EQ(test::read_file(logs / "tsfs.log.1"), "precious log data\n");
    EXPECT_FALSE(fs::exists(logs / "tsfs.log.1.gz"));
    EXPECT_EQ(compressor.failed(), 1U);
    const auto drained = notices->drain();
    ASSERT_FALSE(drained.empty());
    EXPECT_EQ(drained.back().event, "log_compression_failed");
}

TEST_F(LoggingTest, RestartRepairsAnInterruptedRotation) {
    const fs::path logs = dir_ / "logs";
    fs::create_directories(logs);
    test::write_file(logs / "tsfs.log.1", "raw copy\n");
    {
        gzFile gz = gzopen((logs / "tsfs.log.1.gz").c_str(), "wb");
        gzputs(gz, "raw copy\n");
        gzclose(gz);
    }
    test::write_file(logs / "tsfs.log.2", "kept\n");
    test::write_file(logs / "tsfs.log.2.gz", "not a gzip stream");
    test::write_file(logs / ".tsfs.log.compress-42.tmp", "partial");
    test::write_file(logs / "tsfs.log.9", "beyond max_files");
    test::write_file(logs / "tsfs.log.old", "not ours");
    ASSERT_TRUE(log_.configure(config()).has_value());
    EXPECT_FALSE(fs::exists(logs / "tsfs.log.1"));  // Valid .gz kept, raw duplicate removed.
    EXPECT_TRUE(fs::exists(logs / "tsfs.log.1.gz"));
    EXPECT_TRUE(fs::exists(logs / "tsfs.log.2"));  // Corrupt .gz removed, raw kept.
    EXPECT_FALSE(fs::exists(logs / "tsfs.log.2.gz"));
    EXPECT_FALSE(fs::exists(logs / ".tsfs.log.compress-42.tmp"));
    EXPECT_FALSE(fs::exists(logs / "tsfs.log.9"));   // max_files = 3.
    EXPECT_TRUE(fs::exists(logs / "tsfs.log.old"));  // Unknown names are never touched.
}

TEST_F(LoggingTest, DiskFullIsCountedWithoutRecursionOrCrash) {
    ASSERT_TRUE(log_.configure(config()).has_value());
    log_.log(LogLevel::info, "test", "before", "");
    // Simulate a full disk: RLIMIT_FSIZE makes write(2) fail with EFBIG.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    struct sigaction previous{};
    ::sigaction(SIGXFSZ, &ignore, &previous);
    rlimit saved{};
    ::getrlimit(RLIMIT_FSIZE, &saved);
    rlimit tiny = saved;
    tiny.rlim_cur = fs::file_size(active());
    ::setrlimit(RLIMIT_FSIZE, &tiny);
    for (int i = 0; i < 50; ++i) {
        log_.log(LogLevel::error, "test", "lost", "");
    }
    ::setrlimit(RLIMIT_FSIZE, &saved);
    ::sigaction(SIGXFSZ, &previous, nullptr);
    EXPECT_GE(log_.stats().lost_writes, 1U);
    log_.log(LogLevel::info, "test", "after", "");
    log_.maintenance();
    const std::string content = test::read_file(active());
    EXPECT_NE(content.find("event=after"), std::string::npos);
    EXPECT_NE(content.find("event=log_write_failed"), std::string::npos);
}

}  // namespace
}  // namespace tsfs
