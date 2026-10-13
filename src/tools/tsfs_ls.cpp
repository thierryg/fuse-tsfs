// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/tools/tsfs_ls.cpp
// Purpose : ls wrapper: inside TSFS mounts, tags every entry with its retention
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// TSFS-LS-WRAPPER-v2 (marker used by the install and uninstall scripts)
//
// Installed as /usr/bin/ls; the system ls is renamed /usr/bin/ls.orig at
// installation (and renamed back at uninstallation). Every listing is produced by
// ls.orig itself, so every ls option keeps its meaning:
//
// - Outside TSFS mounts, or when standard output is not a terminal, or with
//   TSFS_LS=off: ls.orig is executed with the unchanged command line.
// - When at least one operand is inside a TSFS mount (fstype fuse.tsfs): ls.orig
//   runs with the user's options followed by "-l --dired", whose trailer gives the exact
//   byte position of every listed name and directory header in the output. Each
//   entry is then tagged:
//     created <date>, filter <rule>, deleted <date> (in <remaining>)
//     created <date>, filter <rule>, deletion pending (expired <date>)
//     UNFILTERED                        not deleted by TSFS (no expiring rule, an
//                                       exclude rule, a directory, a link...)
//   The retention comes from the read-only extended attributes served by the TSFS
//   daemon (user.tsfs.state / rule / expires / created). With a long format (-l,
//   -n, -g, -o, --format=long|verbose) the ls.orig lines are kept and the tag is
//   appended; with another format the entries are listed one per line followed by
//   their tag (a column layout cannot carry it).
// TSFS_LS=always also tags when the output is not a terminal; TSFS_REAL_LS
// overrides the path of the original ls.
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TSFS_SYSTEM_LS
#define TSFS_SYSTEM_LS "/usr/bin/ls"
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kMarker = "TSFS-LS-WRAPPER-v2";
constexpr std::string_view kOrigSuffix = ".orig";

/// @return true when @p file is this wrapper (content marker): never delegate to itself.
bool is_wrapper(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), {});
    return content.find(kMarker) != std::string::npos;
}

/// The original ls: $TSFS_REAL_LS, <system ls>.orig, then the PATH (tsfs-ls used
/// under its own name, the system ls untouched).
std::optional<std::string> original_ls() {
    if (const char* env = std::getenv("TSFS_REAL_LS"); env != nullptr && ::access(env, X_OK) == 0) {
        return env;
    }
    if (std::string orig = std::string(TSFS_SYSTEM_LS) + std::string(kOrigSuffix); ::access(orig.c_str(), X_OK) == 0) {
        return orig;
    }
    const char* path = std::getenv("PATH");
    std::string_view dirs = path != nullptr ? path : "/usr/bin:/bin";
    while (!dirs.empty()) {
        const std::size_t colon = dirs.find(':');
        std::string candidate = std::string(dirs.substr(0, colon)) + "/ls";
        dirs = colon == std::string_view::npos ? std::string_view{} : dirs.substr(colon + 1);
        if (::access(candidate.c_str(), X_OK) == 0 && !is_wrapper(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// argv for the original ls: argv[0] stays "ls" (multi-call binaries such as uutils
/// coreutils select the utility from it), then the user's arguments, with @p extra
/// inserted AFTER the user's options (before a "--"): the last format option wins in
/// GNU ls, so "-C"/"-x"/"-m" cannot cancel the "-l" that --dired needs.
std::vector<char*> ls_argv(int argc, char** argv, std::vector<std::string>& storage,
                           std::initializer_list<const char*> extra) {
    storage.assign({"ls"});
    bool inserted = extra.size() == 0;
    for (int i = 1; i < argc; ++i) {
        if (!inserted && std::string_view(argv[i]) == "--") {
            storage.insert(storage.end(), extra.begin(), extra.end());
            inserted = true;
        }
        storage.emplace_back(argv[i]);
    }
    if (!inserted) {
        storage.insert(storage.end(), extra.begin(), extra.end());
    }
    std::vector<char*> out;
    out.reserve(storage.size() + 1);
    for (auto& s : storage) {
        out.push_back(s.data());
    }
    out.push_back(nullptr);
    return out;
}

[[noreturn]] void run_original(const std::optional<std::string>& ls, int argc, char** argv) {
    if (!ls) {
        std::println(stderr, "ls: the original ls cannot be found ({}{} missing; set TSFS_REAL_LS)", TSFS_SYSTEM_LS,
                     kOrigSuffix);
        std::exit(2);
    }
    std::vector<std::string> storage;
    auto args = ls_argv(argc, argv, storage, {});
    ::execv(ls->c_str(), args.data());
    std::println(stderr, "ls: cannot run {}: {}", *ls, std::strerror(errno));
    std::exit(2);
}

/// What the wrapper needs to know about the command line (ls.orig does the rest).
struct CommandLine {
    std::vector<std::string> operands;
    bool long_format{false};
    bool directory{false};    ///< -d / --directory: directories listed as themselves.
    bool passthrough{false};  ///< --help, --version, --zero, --dired, -D: ls.orig alone.
};

CommandLine scan(int argc, char** argv) {
    // Long options of GNU and uutils ls that accept their value as the NEXT argument.
    static constexpr std::string_view kLongWithValue[] = {
        "--block-size", "--format", "--hide",       "--ignore", "--indicator-style", "--quoting-style", "--sort",
        "--tabsize",    "--time",   "--time-style", "--width"};
    CommandLine cl;
    bool operands_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (operands_only || a == "-" || !a.starts_with("-")) {
            cl.operands.emplace_back(a);
            continue;
        }
        if (a == "--") {
            operands_only = true;
            continue;
        }
        if (a.starts_with("--")) {
            const std::string_view name = a.substr(0, a.find('='));
            if (name == "--help" || name == "--version" || name == "--zero" || name == "--dired") {
                cl.passthrough = true;
            }
            if (name == "--directory") {
                cl.directory = true;
            }
            if (a.starts_with("--format=")) {
                const std::string_view f = a.substr(9);
                cl.long_format = f == "long" || f == "verbose";
            }
            if (a.find('=') == std::string_view::npos &&
                std::ranges::find(kLongWithValue, name) != std::end(kLongWithValue) && i + 1 < argc) {
                if (name == "--format") {
                    const std::string_view f = argv[i + 1];
                    cl.long_format = f == "long" || f == "verbose";
                }
                ++i;  // The value is not an operand.
            }
            continue;
        }
        for (std::size_t k = 1; k < a.size(); ++k) {
            const char c = a[k];
            if (c == 'l' || c == 'n' || c == 'g' || c == 'o') {
                cl.long_format = true;
            } else if (c == '1' || c == 'C' || c == 'x' || c == 'm') {
                cl.long_format = false;
            } else if (c == 'D') {
                cl.passthrough = true;
            } else if (c == 'd') {
                cl.directory = true;
            } else if (c == 'I' || c == 'T' || c == 'w') {
                if (k + 1 == a.size() && i + 1 < argc) {
                    ++i;  // "-I PATTERN": the value is the next argument.
                }
                break;  // "-IPATTERN": the rest of the cluster is the value.
            }
        }
    }
    if (cl.operands.empty()) {
        cl.operands.emplace_back(".");
    }
    return cl;
}

/// Mount points of the TSFS filesystems (fstype fuse.tsfs).
std::vector<std::string> tsfs_mounts() {
    std::vector<std::string> mounts;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t dash = line.find(" - ");
        if (dash == std::string::npos || !line.substr(dash + 3).starts_with("fuse.tsfs ")) {
            continue;
        }
        std::size_t pos = 0;
        for (int field = 0; field < 4; ++field) {
            pos = line.find(' ', pos) + 1;
        }
        const std::string raw = line.substr(pos, line.find(' ', pos) - pos);
        std::string mount;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size()) {  // \040 and friends.
                mount.push_back(static_cast<char>(std::strtol(raw.substr(i + 1, 3).c_str(), nullptr, 8)));
                i += 3;
            } else {
                mount.push_back(raw[i]);
            }
        }
        mounts.push_back(mount);
    }
    return mounts;
}

bool inside(const std::string& path, const std::vector<std::string>& mounts) {
    std::error_code ec;
    const std::string p = fs::weakly_canonical(fs::absolute(path, ec), ec).string();
    return std::ranges::any_of(
        mounts, [&](const std::string& m) { return p == m || (p.starts_with(m) && (m == "/" || p[m.size()] == '/')); });
}

/// @return the value, or "" (with errno set by lgetxattr when it failed).
std::string xattr(const std::string& path, const char* name) {
    char buffer[256];
    const ssize_t n = ::lgetxattr(path.c_str(), name, buffer, sizeof buffer);
    return n > 0 ? std::string(buffer, static_cast<std::size_t>(n)) : std::string();
}

/// "2026-10-14T11:51:22Z" -> local "2026-10-14 13:51" and the time_t.
std::optional<std::pair<std::string, std::time_t>> local_time(const std::string& iso) {
    std::tm tm{};
    if (::strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) {
        return std::nullopt;
    }
    const std::time_t when = ::timegm(&tm);
    std::tm local{};
    ::localtime_r(&when, &local);
    char text[32];
    if (std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local) == 0) {
        return std::nullopt;
    }
    return std::pair{std::string(text), when};
}

std::string remaining(long long seconds) {
    const long long d = seconds / 86400;
    const long long h = seconds % 86400 / 3600;
    const long long m = seconds % 3600 / 60;
    if (d > 0) {
        return std::format("{}d {}h", d, h);
    }
    if (h > 0) {
        return std::format("{}h {}m", h, m);
    }
    return m > 0 ? std::format("{}m", m) : std::format("{}s", std::max(0LL, seconds));
}

/// The TSFS tag of one entry.
std::string tag(const std::string& path) {
    errno = 0;
    const std::string state = xattr(path, "user.tsfs.state");
    if (state.empty() && errno == EOPNOTSUPP) {  // ENOTSUP is the same value on Linux.
        // The daemon serves no retention attribute (filesystem.retention_xattrs: false).
        return "RETENTION UNKNOWN (filesystem.retention_xattrs is false)";
    }
    if (state != "expires" && state != "expired") {
        // never, excluded, a directory, a link, a special file: TSFS never deletes it.
        return "UNFILTERED";
    }
    const std::string rule = xattr(path, "user.tsfs.rule");
    const auto created = local_time(xattr(path, "user.tsfs.created"));
    const auto expires = local_time(xattr(path, "user.tsfs.expires"));
    const std::string created_text = created ? created->first : "?";
    if (!expires) {
        return std::format("created {}, filter {}", created_text, rule);
    }
    if (state == "expired") {
        return std::format("created {}, filter {}, deletion pending (expired {})", created_text, rule, expires->first);
    }
    return std::format("created {}, filter {}, deleted {} (in {})", created_text, rule, expires->first,
                       remaining(static_cast<long long>(expires->second - std::time(nullptr))));
}

/// Control characters become '?' (ls on a terminal does the same): a crafted file
/// name must not send escape sequences to the terminal.
std::string printable(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            c = '?';
        }
    }
    return out;
}

/// Run ls.orig with @p args, standard output captured. @return its exit status.
int capture(const std::string& ls, std::vector<char*>& args, std::string& output) {
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
        return 127;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return 127;
    }
    if (pid == 0) {
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::execv(ls.c_str(), args.data());
        ::_exit(127);
    }
    ::close(pipefd[1]);
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(pipefd[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

/// The numbers of a "//DIRED// 12 18 ..." line.
std::vector<std::size_t> offsets(std::string_view line) {
    std::vector<std::size_t> values;
    std::size_t pos = line.find(' ');
    while (pos != std::string_view::npos) {
        const std::size_t next = line.find(' ', pos + 1);
        const std::string number(line.substr(pos + 1, next == std::string_view::npos ? next : next - pos - 1));
        if (!number.empty()) {
            values.push_back(std::strtoull(number.c_str(), nullptr, 10));
        }
        pos = next;
    }
    return values;
}

/// Print the --dired output of ls.orig with the TSFS tags. @return false when
/// @p out has no dired trailer (then it is printed unchanged by the caller).
bool annotate(const std::string& out, const CommandLine& cl, bool keep_long) {
    // Trailer: "//DIRED// s e s e ...", optional "//SUBDIRED// ...", "//DIRED-OPTIONS// ...".
    std::size_t trailer = out.starts_with("//DIRED//") ? 0 : out.find("\n//DIRED//");
    if (trailer == std::string::npos) {
        return false;
    }
    trailer += trailer == 0 ? 0 : 1;
    std::vector<std::size_t> names;
    std::vector<std::size_t> subdirs;
    for (std::size_t pos = trailer; pos < out.size();) {
        const std::size_t end = std::min(out.find('\n', pos), out.size());
        const std::string_view line(out.data() + pos, end - pos);
        if (line.starts_with("//DIRED// ")) {
            names = offsets(line);
        } else if (line.starts_with("//SUBDIRED// ")) {
            subdirs = offsets(line);
        }
        pos = end + 1;
    }
    // Directory of the entries: the latest header, else the single directory operand
    // (no header is printed for it), else the current directory (file operands).
    std::string base;
    if (subdirs.empty() && cl.operands.size() == 1 && !cl.directory) {
        std::error_code ec;
        if (fs::is_directory(cl.operands.front(), ec) && !fs::is_symlink(cl.operands.front(), ec)) {
            base = cl.operands.front();
        }
    }
    std::size_t next_name = 0;
    std::size_t next_subdir = 0;
    for (std::size_t pos = 0; pos < trailer;) {
        const std::size_t end = std::min(out.find('\n', pos), trailer);
        const std::string_view line(out.data() + pos, end - pos);
        const std::size_t line_start = pos;
        pos = end + 1;
        const std::string_view body = line.starts_with("  ") ? line.substr(2) : line;  // dired indent.
        if (next_subdir + 1 < subdirs.size() && subdirs[next_subdir] >= line_start && subdirs[next_subdir] < end) {
            base = out.substr(subdirs[next_subdir], subdirs[next_subdir + 1] - subdirs[next_subdir]);
            next_subdir += 2;
            std::println("{}", printable(body));
            continue;
        }
        if (next_name + 1 < names.size() && names[next_name] >= line_start && names[next_name] < end) {
            const std::string name = out.substr(names[next_name], names[next_name + 1] - names[next_name]);
            next_name += 2;
            const std::string path = base.empty() ? name : (fs::path(base) / name).string();
            std::println("{}  {}", printable(keep_long ? body : std::string_view(name)), tag(path));
            continue;
        }
        if (!keep_long && body.starts_with("total ")) {
            continue;  // Only meaningful with the long format.
        }
        std::println("{}", printable(body));
    }
    return true;
}

int run(int argc, char** argv) {
    (void)std::setlocale(LC_ALL, "");
    const auto ls = original_ls();
    const char* env = std::getenv("TSFS_LS");
    const std::string_view mode = env != nullptr ? env : "";
    const CommandLine cl = scan(argc, argv);
    if (mode == "off" || cl.passthrough || (mode != "always" && ::isatty(STDOUT_FILENO) == 0) || !ls) {
        run_original(ls, argc, argv);
    }
    const auto mounts = tsfs_mounts();
    if (std::ranges::none_of(cl.operands, [&](const std::string& o) { return inside(o, mounts); })) {
        run_original(ls, argc, argv);
    }
    std::vector<std::string> storage;
    auto args = ls_argv(argc, argv, storage, {"-l", "--dired"});
    std::string output;
    const int status = capture(*ls, args, output);
    if (status == 127 || !annotate(output, cl, cl.long_format)) {
        (void)std::fwrite(output.data(), 1, output.size(), stdout);  // Unexpected format: unchanged.
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape): std::println cannot throw here.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "ls: {}", e.what());
        return 2;
    }
}
