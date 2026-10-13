// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/metrics/status_report.cpp
// Purpose : Runtime status document (JSON) assembled from every component, written atomically
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "metrics/status_report.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <iterator>

#include "common/escape.hpp"
#include "filesystem/fuse_control.hpp"

namespace tsfs {
namespace {

std::string json_string(std::string_view text) {
    std::string out;
    append_json_quoted(out, text);
    return out;
}

}  // namespace

std::string render_status_json(const StatusInputs& in) {
    std::string out;
    auto it = std::back_inserter(out);
    std::format_to(it, "{{\n  \"version\": {},\n  \"pid\": {},\n  \"uptime_s\": {},\n", json_string(in.version), in.pid,
                   (in.now_unix_ms - in.started_unix_ms) / 1000);
    std::format_to(it, "  \"updated_unix_ms\": {},\n  \"mountpoint\": {},\n  \"backing_directory\": {},\n",
                   in.now_unix_ms, json_string(in.mountpoint), json_string(in.backing_directory));
    std::format_to(it, "  \"config\": {{\"generation\": {}, \"reloads\": {}, \"reload_failures\": {}}},\n",
                   in.config_generation, in.config_reloads, in.config_reload_failures);
    const GcStats& g = in.gc;
    std::format_to(
        it,
        "  \"gc\": {{\"candidates\": {}, \"deleted\": {}, \"bytes_deleted\": {}, \"would_delete\": {}, "
        "\"vanished\": {}, \"skipped_modified\": {}, \"skipped_replaced\": {}, \"skipped_open\": {}, "
        "\"skipped_unsafe\": {}, \"failed\": {}, \"retries\": {}, \"queue_saturations\": {}, "
        "\"events_dropped\": {}, \"scans_completed\": {}, \"managed_files\": {}, "
        "\"last_scan_duration_ms\": {}, \"last_reconciliation_unix_ms\": {}, \"scan_time_ms\": {}, "
        "\"delete_time_us\": {}, \"worker_busy_us\": {}, \"scheduled\": {}, \"pending_work\": {}, "
        "\"workers\": {}, \"purges_completed\": {}, \"purging\": {}, "
        "\"last_purge\": {{\"deleted\": {}, \"bytes_deleted\": {}, \"failed\": {}, \"skipped_open\": {}}}}},\n",
        g.candidates, g.deleted, g.bytes_deleted, g.would_delete, g.vanished, g.skipped_modified, g.skipped_replaced,
        g.skipped_open, g.skipped_unsafe, g.failed, g.retries, g.queue_saturations, g.events_dropped, g.scans_completed,
        g.last_scan_files, g.last_scan_duration_ms, g.last_scan_completed_unix_ms, g.scan_time_ms, g.delete_time_us,
        g.worker_busy_us, g.scheduled, g.pending_work, g.workers, g.purges_completed, g.purging, g.last_purge_deleted,
        g.last_purge_bytes, g.last_purge_failed, g.last_purge_skipped_open);
    out += "  \"filesystem\": {\"operations\": {";
    for (std::size_t i = 0; i < kFsOpCount; ++i) {
        std::format_to(it, "{}\"{}\": {{\"calls\": {}, \"errors\": {}}}", i == 0 ? "" : ", ",
                       to_string(static_cast<FsOp>(i)), in.fs.calls[i], in.fs.errors[i]);
    }
    std::format_to(it, "}}, \"bytes_read\": {}, \"bytes_written\": {}, \"open_handles\": {}}},\n", in.fs.bytes_read,
                   in.fs.bytes_written, in.fs.open_handles);
    // The kernel side of the mount: /sys/fs/fuse/connections/<id>/ (fusectl).
    const auto json_or_null = [](const auto& value) { return value ? std::format("{}", *value) : std::string("null"); };
    std::format_to(it, "  \"fuse\": {{\"connection_id\": {}, \"control_dir\": {}, \"waiting\": {}}},\n",
                   json_or_null(in.fuse_connection),
                   in.fuse_connection ? json_string(std::format("{}/{}", kFuseConnectionsDir, *in.fuse_connection))
                                      : std::string("null"),
                   json_or_null(in.fuse_waiting));
    const LoggerStats& l = in.logging;
    std::format_to(it,
                   "  \"logging\": {{\"queue_overruns\": {}, \"lost_writes\": {}, \"rotations\": {}, "
                   "\"reconfigurations\": {}, \"notices_dropped\": {}}}\n}}\n",
                   l.queue_overruns, l.lost_writes, l.rotations, l.reconfigurations, l.notices_dropped);
    return out;
}

Result<void> write_file_atomically(const std::string& path, const std::string& content) {
    const std::size_t slash = path.rfind('/');
    const std::string directory = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    if (::mkdir(directory.c_str(), 0750) != 0 && errno != EEXIST) {
        const int err = errno;
        return make_error(err, std::format("cannot create '{}': {}", directory, std::strerror(err)));
    }
    // Unpredictable name created with O_EXCL (mkostemp): never opens an existing file
    // or a link planted in the directory, and concurrent writers never share it.
    std::string tmp = path + ".tmp.XXXXXX";
    const int fd = ::mkostemp(tmp.data(), O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return make_error(err, std::format("cannot create '{}': {}", tmp, std::strerror(err)));
    }
    if (::fchmod(fd, 0640) != 0) {  // mkostemp creates 0600; the status is group-readable.
        const int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        return make_error(err, std::format("cannot chmod '{}': {}", tmp, std::strerror(err)));
    }
    std::string_view rest = content;
    while (!rest.empty()) {
        const ssize_t n = ::write(fd, rest.data(), rest.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            const int err = n < 0 ? errno : EIO;
            ::close(fd);
            ::unlink(tmp.c_str());
            return make_error(err, std::format("cannot write '{}': {}", tmp, std::strerror(err)));
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        return make_error(err, std::format("cannot rename '{}': {}", tmp, std::strerror(err)));
    }
    return {};
}

}  // namespace tsfs
