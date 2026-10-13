// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/diagnostics/crash_handler.cpp
// Purpose : Fatal-signal text reports (async-signal-safe) and native core-dump enabling
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "diagnostics/crash_handler.hpp"

#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>
#include <ranges>

#include "common/threads.hpp"
#include "common/unique_fd.hpp"
#include "diagnostics/symbol_table.hpp"

namespace tsfs::diagnostics {
namespace {

constexpr std::array kFatalSignals{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP};
constexpr int kMaxFrames = 64;

// --- process-wide state read by the handler (written before installation only) ---
int g_dir_fd = -1;
std::array<char, 64> g_version{};
std::array<struct sigaction, kFatalSignals.size()> g_previous{};
std::atomic<bool> g_installed{false};
std::atomic<int> g_handling{0};
// Function names resolved at crash time (built at install, immutable, never freed
// while a handler may run: see SymbolTable).
std::unique_ptr<const SymbolTable> g_symbols_owner;
std::atomic<const SymbolTable*> g_symbols{nullptr};

// --- async-signal-safe output helpers (no allocation, no locale, no stdio) ---------
void put(int fd, const char* text) noexcept {
    if (fd < 0) {
        return;
    }
    std::size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    while (length > 0) {
        const ssize_t n = ::write(fd, text, length);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        text += n;
        length -= static_cast<std::size_t>(n);
    }
}

void put_unsigned(int fd, unsigned long long value, unsigned base = 10, int min_digits = 1) noexcept {
    std::array<char, 32> buffer{};
    std::size_t pos = buffer.size() - 1;
    buffer[pos] = '\0';
    int digits = 0;
    do {
        const unsigned digit = static_cast<unsigned>(value % base);
        buffer[--pos] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= base;
        ++digits;
    } while ((value != 0 || digits < min_digits) && pos > 0);
    put(fd, &buffer[pos]);
}

void put_hex(int fd, std::uintptr_t value) noexcept {
    put(fd, "0x");
    put_unsigned(fd, value, 16, 1);
}

const char* signal_name(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGSYS: return "SIGSYS";
        case SIGTRAP: return "SIGTRAP";
        default: return "unknown";
    }
}

/// Civil date from a Unix time (Howard Hinnant's algorithm): pure arithmetic.
void put_utc(int fd, long long seconds) noexcept {
    const long long days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    long long rem = seconds - days * 86400;
    long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
    put_unsigned(fd, static_cast<unsigned long long>(y), 10, 4);
    put(fd, "-");
    put_unsigned(fd, static_cast<unsigned long long>(m), 10, 2);
    put(fd, "-");
    put_unsigned(fd, static_cast<unsigned long long>(d), 10, 2);
    put(fd, "T");
    put_unsigned(fd, static_cast<unsigned long long>(rem / 3600), 10, 2);
    rem %= 3600;
    put(fd, ":");
    put_unsigned(fd, static_cast<unsigned long long>(rem / 60), 10, 2);
    put(fd, ":");
    put_unsigned(fd, static_cast<unsigned long long>(rem % 60), 10, 2);
    put(fd, "Z");
}

std::uintptr_t instruction_pointer(void* context) noexcept {
    if (context == nullptr) {
        return 0;
    }
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#else
    (void)uc;
    return 0;
#endif
}

void copy_file(int out, const char* path) noexcept {
    const int in = ::open(path, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        put(out, "(unavailable)\n");
        return;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t n = ::read(in, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(out, buffer.data() + off, static_cast<std::size_t>(n - off));
            if (w <= 0) {
                break;
            }
            off += w;
        }
    }
    ::close(in);
}

/// Frames captured once by the handler, shared by the report and stderr.
struct Backtrace {
    std::array<void*, kMaxFrames> frames{};
    int count{0};
    int first{0};  ///< Index of the faulting frame (handler frames come before it).
};

Backtrace capture_backtrace(void* context) noexcept {
    Backtrace bt;
    bt.count = ::backtrace(bt.frames.data(), kMaxFrames);
    const std::uintptr_t ip = instruction_pointer(context);
    for (int i = 0; i < bt.count; ++i) {
        if (reinterpret_cast<std::uintptr_t>(bt.frames[static_cast<std::size_t>(i)]) == ip) {
            bt.first = i;
            break;
        }
    }
    return bt;
}

/// "  #NN 0xADDR function+0xOFF (module)" for frames [from, count). Async-signal-safe:
/// the names come from the preloaded SymbolTable.
void put_symbolized_frames(int fd, const Backtrace& bt, int from) noexcept {
    const SymbolTable* symbols = g_symbols.load();
    for (int i = from; i < bt.count; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(bt.frames[static_cast<std::size_t>(i)]);
        put(fd, "  #");
        put_unsigned(fd, static_cast<unsigned>(i - from), 10, 2);
        put(fd, " ");
        put_hex(fd, address);
        // A return address points after the call instruction: resolve the call itself.
        // The faulting frame holds the exact instruction pointer.
        const std::uintptr_t lookup = i == bt.first ? address : address - 1;
        SymbolTable::Match match;
        if (symbols != nullptr && symbols->lookup(lookup, match)) {
            put(fd, " ");
            put(fd, match.function);
            put(fd, "+");
            put_hex(fd, address - (lookup - match.offset));
            put(fd, " (");
            put(fd, match.module);
            put(fd, ")\n");
        } else if (const char* module = nullptr; symbols != nullptr && symbols->locate(lookup, module, match.offset)) {
            // Internal function of a stripped library: module-relative address, for
            // offline symbolization with that library's debug symbols.
            put(fd, " ?? (");
            put(fd, module);
            put(fd, "+");
            put_hex(fd, match.offset + (address - lookup));
            put(fd, ")\n");
        } else {
            put(fd, " ??\n");
        }
    }
}

/// Writes the report to @p fd. Every call below is async-signal-safe (see header).
void write_report(int fd, int sig, const siginfo_t* info, void* context, const Backtrace& bt) noexcept {
    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    put(fd, "TSFS crash report\n");
    put(fd, "version: ");
    put(fd, g_version.data());
    put(fd, "\nsignal: ");
    put_unsigned(fd, static_cast<unsigned>(sig));
    put(fd, " (");
    put(fd, signal_name(sig));
    put(fd, ")\ncode: ");
    if (info != nullptr && info->si_code < 0) {
        put(fd, "-");
        put_unsigned(fd, static_cast<unsigned long long>(-info->si_code));
    } else {
        put_unsigned(fd, info != nullptr ? static_cast<unsigned long long>(info->si_code) : 0);
    }
    put(fd, "\npid: ");
    put_unsigned(fd, static_cast<unsigned long long>(::getpid()));
    put(fd, "\ntid: ");
    put_unsigned(fd, static_cast<unsigned long long>(::syscall(SYS_gettid)));
    put(fd, "\ntime: ");
    put_utc(fd, now.tv_sec);
    put(fd, " (unix ");
    put_unsigned(fd, static_cast<unsigned long long>(now.tv_sec));
    put(fd, ")\nfault_address: ");
    put_hex(fd, info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
    put(fd, "\ninstruction_pointer: ");
    put_hex(fd, instruction_pointer(context));
    put(fd, "\n\nbacktrace (raw addresses):\n");
    for (int i = 0; i < bt.count; ++i) {
        put(fd, "  #");
        put_unsigned(fd, static_cast<unsigned>(i), 10, 2);
        put(fd, " ");
        put_hex(fd, reinterpret_cast<std::uintptr_t>(bt.frames[static_cast<std::size_t>(i)]));
        put(fd, "\n");
    }
    put(fd, "\nbacktrace (symbols, from the faulting frame; file:line: scripts/tsfs-symbolize.sh):\n");
    if (g_symbols.load() != nullptr) {
        put_symbolized_frames(fd, bt, bt.first);
    } else {
        ::backtrace_symbols_fd(bt.frames.data() + bt.first, bt.count - bt.first, fd);
    }
    put(fd, "\nmemory map (/proc/self/maps):\n");
    copy_file(fd, "/proc/self/maps");
    put(fd, "\nend of report\n");
}

void fatal_handler(int sig, siginfo_t* info, void* context) {
    const int saved_errno = errno;
    if (g_handling.exchange(1) != 0) {
        // Another thread is writing the report and will terminate the process.
        for (;;) {
            ::pause();
        }
    }
    int fd = -1;
    std::array<char, 96> name{};
    if (g_dir_fd >= 0) {
        // "tsfs-crash-<unix>-<pid>.txt", built without snprintf.
        timespec now{};
        ::clock_gettime(CLOCK_REALTIME, &now);
        std::size_t pos = 0;
        const auto append = [&](const char* text) {
            while (*text != '\0' && pos + 1 < name.size()) {
                name[pos++] = *text++;
            }
        };
        const auto append_number = [&](unsigned long long v) {
            std::array<char, 24> digits{};
            std::size_t n = 0;
            do {
                digits[n++] = static_cast<char>('0' + v % 10);
                v /= 10;
            } while (v != 0 && n < digits.size());
            while (n > 0 && pos + 1 < name.size()) {
                name[pos++] = digits[--n];
            }
        };
        append("tsfs-crash-");
        append_number(static_cast<unsigned long long>(now.tv_sec));
        append("-");
        append_number(static_cast<unsigned long long>(::getpid()));
        append(".txt");
        fd = ::openat(g_dir_fd, name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    const Backtrace bt = capture_backtrace(context);
    // The logger cannot be used in a signal handler: the diagnostic events go to stderr
    // (the journal under systemd) with the same event identifiers as the log.
    put(STDERR_FILENO, "tsfs: fatal signal ");
    put(STDERR_FILENO, signal_name(sig));
    if (fd >= 0) {
        put(STDERR_FILENO, ", crash report: ");
        put(STDERR_FILENO, name.data());
        put(STDERR_FILENO, " event=diagnostic_report_started\n");
    } else {
        put(STDERR_FILENO, ", no crash report (cannot create it) event=diagnostic_report_failed\n");
    }
    put(STDERR_FILENO, "tsfs: backtrace:\n");
    if (g_symbols.load() != nullptr) {
        put_symbolized_frames(STDERR_FILENO, bt, bt.first);
    } else {
        ::backtrace_symbols_fd(bt.frames.data() + bt.first, bt.count - bt.first, STDERR_FILENO);
    }
    if (fd >= 0) {
        write_report(fd, sig, info, context, bt);
        ::fsync(fd);
        ::close(fd);
        put(STDERR_FILENO, "tsfs: crash report written: ");
        put(STDERR_FILENO, name.data());
        put(STDERR_FILENO, " event=diagnostic_report_completed\n");
    }
    // Restore the default action and let the signal terminate the process (core dump
    // included when enabled). A fault raised by the kernel is re-raised by returning:
    // the faulting instruction executes again with the default disposition.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(sig, &dfl, nullptr);
    errno = saved_errno;
    if (info == nullptr || info->si_code <= 0 || sig == SIGABRT) {
        (void)::raise(sig);  // Nothing to do on failure: the handler is already SIG_DFL.
    }
}

int make_directories(const std::string& path) {
    std::size_t pos = 1;
    while (pos <= path.size()) {
        const std::size_t end = std::min(path.find('/', pos), path.size());
        if (::mkdir(path.substr(0, end).c_str(), 0750) != 0 && errno != EEXIST) {
            return errno;
        }
        pos = end + 1;
    }
    return 0;
}

}  // namespace

Result<void> CrashHandler::install(const CrashReportOptions& options) {
    if (g_installed.load()) {
        return {};
    }
    if (const int err = make_directories(options.directory); err != 0) {
        return make_error(err, "cannot create the crash directory '" + options.directory + "': " + std::strerror(err));
    }
    const int fd = ::open(options.directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return make_error(err, "cannot open the crash directory '" + options.directory + "': " + std::strerror(err));
    }
    prune_reports(options.directory, options.max_reports > 0 ? options.max_reports - 1 : 0);
    g_dir_fd = fd;
    const std::size_t n = std::min(options.version.size(), g_version.size() - 1);
    std::copy_n(options.version.begin(), n, g_version.begin());
    g_version[n] = '\0';

    // backtrace() loads libgcc_s lazily on first use (malloc + dlopen): do it now,
    // outside any signal handler.
    std::array<void*, 2> warmup{};
    ::backtrace(warmup.data(), static_cast<int>(warmup.size()));
    // Function names for the crash-time backtrace: loaded now, looked up in the handler.
    if (g_symbols.load() == nullptr) {
        g_symbols_owner = SymbolTable::load_current_process();
        g_symbols.store(g_symbols_owner.get());
    }

    install_alternate_signal_stack();
    for (std::size_t i = 0; i < kFatalSignals.size(); ++i) {
        struct sigaction action{};
        action.sa_sigaction = fatal_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        ::sigaction(kFatalSignals[i], &action, &g_previous[i]);
    }
    g_installed.store(true);
    return {};
}

void CrashHandler::uninstall() noexcept {
    if (!g_installed.exchange(false)) {
        return;
    }
    for (std::size_t i = 0; i < kFatalSignals.size(); ++i) {
        ::sigaction(kFatalSignals[i], &g_previous[i], nullptr);
    }
    if (g_dir_fd >= 0) {
        ::close(g_dir_fd);
        g_dir_fd = -1;
    }
}

bool CrashHandler::installed() noexcept { return g_installed.load(); }

std::size_t CrashHandler::symbol_count() noexcept {
    const SymbolTable* symbols = g_symbols.load();
    return symbols != nullptr ? symbols->size() : 0;
}

std::size_t CrashHandler::prune_reports(const std::string& directory, std::uint32_t keep) {
    DIR* dir = ::opendir(directory.c_str());
    if (dir == nullptr) {
        return 0;
    }
    const int dfd = ::dirfd(dir);
    if (dfd < 0) {
        ::closedir(dir);
        return 0;
    }
    std::vector<std::pair<long long, std::string>> reports;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (!name.starts_with("tsfs-crash-") || !name.ends_with(".txt")) {
            continue;  // Only TSFS reports are ever removed; core files are left alone.
        }
        struct stat st{};
        if (::fstatat(dfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)) {
            reports.emplace_back(st.st_mtim.tv_sec, std::string(name));
        }
    }
    std::ranges::sort(reports, std::greater<>{});
    std::size_t removed = 0;
    for (const auto& [_, name] : reports | std::views::drop(keep)) {
        if (::unlinkat(dfd, name.c_str(), 0) == 0) {
            ++removed;
        }
    }
    ::closedir(dir);
    return removed;
}

CoreDumpStatus inspect_core_dumps() {
    CoreDumpStatus status;
    rlimit limit{};
    if (::getrlimit(RLIMIT_CORE, &limit) == 0) {
        status.soft_limit = limit.rlim_cur;
        status.hard_limit = limit.rlim_max;
    }
    status.dumpable = ::prctl(PR_GET_DUMPABLE) == 1;
    std::ifstream pattern("/proc/sys/kernel/core_pattern");
    std::getline(pattern, status.core_pattern);
    if (status.core_pattern.find("systemd-coredump") != std::string::npos) {
        status.handler = "systemd-coredump";
        status.notes.emplace_back("cores are collected by systemd-coredump: coredumpctl list tsfs");
    } else if (status.core_pattern.find("apport") != std::string::npos) {
        status.handler = "apport";
        status.notes.emplace_back("cores go to apport (/var/crash); apport ignores non-packaged binaries by default");
    } else if (status.core_pattern.starts_with("|")) {
        status.handler = "pipe";
    } else {
        status.handler = "file";
        if (!status.core_pattern.starts_with("/")) {
            status.notes.emplace_back(
                "relative core_pattern: the core is written to the working directory, which is '/' for a daemon "
                "(usually not writable)");
        }
    }
    if (status.hard_limit == 0) {
        status.notes.emplace_back("hard RLIMIT_CORE is 0: raise it (systemd LimitCORE=infinity, ulimit -Hc)");
    }
    status.enabled = status.soft_limit > 0 && status.dumpable;
    return status;
}

CoreDumpStatus enable_core_dumps() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_CORE, &limit) == 0 && limit.rlim_max > 0) {
        limit.rlim_cur = limit.rlim_max;
        ::setrlimit(RLIMIT_CORE, &limit);
    }
    ::prctl(PR_SET_DUMPABLE, 1);
    return inspect_core_dumps();
}

}  // namespace tsfs::diagnostics
