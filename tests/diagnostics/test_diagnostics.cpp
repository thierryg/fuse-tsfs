// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/diagnostics/test_diagnostics.cpp
// Purpose : Crash reports and core dumps, verified on a crashing subprocess
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <fcntl.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "diagnostics/crash_handler.hpp"
#include "diagnostics/symbol_table.hpp"
#include "support/test_support.hpp"

extern char** environ;

namespace tsfs {
namespace {

namespace fs = std::filesystem;

struct ChildResult {
    int status{0};
    std::string output;
};

/// Run the crash helper in its own process (the test runner never crashes).
ChildResult run_helper(const std::vector<std::string>& args, const fs::path& cwd) {
    std::vector<std::string> all{TSFS_CRASH_HELPER};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    const fs::path out = cwd / "helper.out";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    EXPECT_EQ(rc, 0);
    ChildResult result;
    ::waitpid(pid, &result.status, 0);
    result.output = test::read_file(out);
    return result;
}

std::vector<fs::path> reports(const fs::path& dir) {
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().starts_with("tsfs-crash-")) {
            found.push_back(e.path());
        }
    }
    return found;
}

/// Keep the artifacts of a failed test under TSFS_TEST_ARTIFACT_DIR when requested.
class DiagnosticsTest : public ::testing::Test {
protected:
    test::TempDir dir_{"tsfs-diag"};
    fs::path crash_dir_{dir_ / "crash"};
};

TEST(SymbolTable, ResolvesFunctionsOfTheExecutableAndItsLibraries) {
    const auto table = diagnostics::SymbolTable::load_current_process();
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->size(), 100U);
    EXPECT_GT(table->objects_loaded(), 1U);  // The executable and at least libc.
    diagnostics::SymbolTable::Match match;
    // A TSFS function (executable, internal symbol table) and a libc one (shared library).
    const auto own = reinterpret_cast<std::uintptr_t>(&diagnostics::CrashHandler::prune_reports);
    ASSERT_TRUE(table->lookup(own + 1, match));
    EXPECT_NE(std::string_view(match.function).find("CrashHandler::prune_reports"), std::string_view::npos)
        << match.function;
    EXPECT_EQ(match.offset, 1U);
    EXPECT_NE(std::string_view(match.module).find("tsfs_diagnostics_tests"), std::string_view::npos) << match.module;
    const auto libc = reinterpret_cast<std::uintptr_t>(&::getpid);
    ASSERT_TRUE(table->lookup(libc, match));
    EXPECT_NE(std::string_view(match.function).find("getpid"), std::string_view::npos) << match.function;
    const char* module = nullptr;
    std::uintptr_t offset = 0;
    EXPECT_TRUE(table->locate(libc, module, offset));
    EXPECT_FALSE(table->lookup(0x10, match));  // Not mapped.
    EXPECT_FALSE(table->locate(0x10, module, offset));
}

TEST_F(DiagnosticsTest, SegfaultWritesACompleteReport) {
    const auto r = run_helper({"segv", crash_dir_.string()}, dir_.path());
    ASSERT_TRUE(WIFSIGNALED(r.status)) << r.output;
    EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
    const auto found = reports(crash_dir_);
    ASSERT_EQ(found.size(), 1U) << r.output;
    const std::string report = test::read_file(found[0]);
    for (const char* expected : {"TSFS crash report", "version: test", "signal: 11 (SIGSEGV)", "pid: ", "tid: ",
                                 "time: ", "fault_address: 0x0", "instruction_pointer: 0x", "backtrace (raw addresses)",
                                 "backtrace (symbols", "memory map (/proc/self/maps)", "end of report"}) {
        EXPECT_NE(report.find(expected), std::string::npos) << expected << "\n" << report;
    }
    EXPECT_NE(report.find("tsfs_crash_helper"), std::string::npos);  // Module named in the frames/maps.
    EXPECT_NE(r.output.find("fatal signal SIGSEGV"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("event=diagnostic_report_started"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("event=diagnostic_report_completed"), std::string::npos) << r.output;
    // Function names at crash time, in the report and on stderr, starting at the
    // faulting function (internal linkage: found through .symtab).
    const auto symbols = report.find("backtrace (symbols");
    ASSERT_NE(symbols, std::string::npos);
    const auto first_frame = report.find("  #00 ", symbols);
    ASSERT_NE(first_frame, std::string::npos) << report;
    const std::string first_line = report.substr(first_frame, report.find('\n', first_frame) - first_frame);
    EXPECT_NE(first_line.find("deliberate_null_write()+0x"), std::string::npos) << first_line;
    EXPECT_NE(first_line.find("tsfs_crash_helper)"), std::string::npos) << first_line;
    EXPECT_NE(report.find("deliberate_null_write()+0x", symbols), std::string::npos) << report;
    EXPECT_NE(report.find(" main+0x", symbols), std::string::npos) << report;
    EXPECT_NE(r.output.find("tsfs: backtrace:"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("deliberate_null_write()+0x"), std::string::npos) << r.output;
    EXPECT_EQ(fs::status(found[0]).permissions() & fs::perms::others_read, fs::perms::none);
}

TEST_F(DiagnosticsTest, AbortIsReportedAndStillTerminatesWithSigabrt) {
    const auto r = run_helper({"abort", crash_dir_.string()}, dir_.path());
    ASSERT_TRUE(WIFSIGNALED(r.status));
    EXPECT_EQ(WTERMSIG(r.status), SIGABRT);
    ASSERT_EQ(reports(crash_dir_).size(), 1U);
    EXPECT_NE(test::read_file(reports(crash_dir_)[0]).find("SIGABRT"), std::string::npos);
}

TEST_F(DiagnosticsTest, RaisedSigfpe) {
    const auto r = run_helper({"fpe", crash_dir_.string()}, dir_.path());
    ASSERT_TRUE(WIFSIGNALED(r.status));
    EXPECT_EQ(WTERMSIG(r.status), SIGFPE);
    ASSERT_EQ(reports(crash_dir_).size(), 1U);
}

TEST_F(DiagnosticsTest, StackOverflowIsReportedFromTheAlternateStack) {
    const auto r = run_helper({"stack", crash_dir_.string()}, dir_.path());
    ASSERT_TRUE(WIFSIGNALED(r.status));
    EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
    ASSERT_EQ(reports(crash_dir_).size(), 1U);
    EXPECT_NE(test::read_file(reports(crash_dir_)[0]).find("SIGSEGV"), std::string::npos);
}

TEST_F(DiagnosticsTest, CrashInAHelperThread) {
    const auto r = run_helper({"thread-segv", crash_dir_.string()}, dir_.path());
    ASSERT_TRUE(WIFSIGNALED(r.status));
    ASSERT_EQ(reports(crash_dir_).size(), 1U);
    EXPECT_NE(r.output.find("deliberate_null_write()+0x"), std::string::npos) << r.output;
}

TEST_F(DiagnosticsTest, NormalExitIsNotDisturbed) {
    const auto r = run_helper({"exit", crash_dir_.string()}, dir_.path());
    ASSERT_TRUE(WIFEXITED(r.status));
    EXPECT_EQ(WEXITSTATUS(r.status), 0);
    EXPECT_TRUE(reports(crash_dir_).empty());
}

TEST_F(DiagnosticsTest, OldReportsArePrunedButNotOtherFiles) {
    fs::create_directories(crash_dir_);
    for (int i = 0; i < 5; ++i) {
        test::write_file(crash_dir_ / ("tsfs-crash-" + std::to_string(1000 + i) + "-1.txt"), "r");
        test::set_mtime(crash_dir_ / ("tsfs-crash-" + std::to_string(1000 + i) + "-1.txt"),
                        test::kEpoch2026 + std::chrono::minutes(i));
    }
    test::write_file(crash_dir_ / "core.1234", "a core file");
    EXPECT_EQ(diagnostics::CrashHandler::prune_reports(crash_dir_.string(), 2), 3U);
    EXPECT_EQ(reports(crash_dir_).size(), 2U);
    EXPECT_TRUE(fs::exists(crash_dir_ / "tsfs-crash-1004-1.txt"));  // Newest kept.
    EXPECT_TRUE(fs::exists(crash_dir_ / "core.1234"));
}

TEST(CoreDumps, InspectionReportsTheEnvironment) {
    const auto status = diagnostics::inspect_core_dumps();
    RecordProperty("core_pattern", status.core_pattern);
    RecordProperty("rlimit_core_soft", std::to_string(status.soft_limit));
    RecordProperty("core_handler", status.handler);
    std::cout << "[ INFO     ] core_pattern='" << status.core_pattern << "' soft=" << status.soft_limit
              << " hard=" << status.hard_limit << " handler=" << status.handler << '\n';
    EXPECT_FALSE(status.handler.empty());
}

TEST_F(DiagnosticsTest, NativeCoreDumpWhenTheEnvironmentAllowsIt) {
    const char* enabled = std::getenv("TSFS_TEST_ENABLE_CORE_DUMPS");
    if (enabled == nullptr || std::string_view(enabled) != "1") {
        GTEST_SKIP() << "set TSFS_TEST_ENABLE_CORE_DUMPS=1 to test native core dumps";
    }
    const auto status = diagnostics::inspect_core_dumps();
    if (status.hard_limit == 0) {
        GTEST_SKIP() << "hard RLIMIT_CORE is 0 in this environment";
    }
    if (status.handler != "file") {
        GTEST_SKIP() << "core_pattern '" << status.core_pattern << "' sends cores to " << status.handler
                     << ": check with coredumpctl/apport, not verifiable from here";
    }
    const auto r = run_helper({"segv", crash_dir_.string(), "core"}, dir_.path());
    ASSERT_TRUE(WIFSIGNALED(r.status));
    EXPECT_TRUE(WCOREDUMP(r.status)) << r.output;
    bool found = false;
    for (const auto& e : fs::directory_iterator(dir_.path())) {
        found |= e.path().filename().string().starts_with("core");
    }
    EXPECT_TRUE(found) << "core_pattern=" << status.core_pattern;
    EXPECT_EQ(reports(crash_dir_).size(), 1U);  // Both modes at once.
}

}  // namespace
}  // namespace tsfs
