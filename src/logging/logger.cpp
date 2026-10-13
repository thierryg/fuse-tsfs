// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/logger.cpp
// Purpose : Structured logging facade: levels, events, async pipeline, hot reconfiguration
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "logging/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <format>
#include <vector>

#include "common/threads.hpp"
#include "logging/log_compressor.hpp"
#include "logging/rotating_file_sink.hpp"

namespace tsfs {

/// Everything one logging configuration needs. The thread pool is joined first: it
/// drains its queue into the sinks (queued messages keep the logger, hence the sinks,
/// alive), so the sinks are destroyed only once no worker thread can touch them.
struct Logger::Pipeline {
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::shared_ptr<LogCompressor> compressor;
    std::shared_ptr<RotatingFileSink> file;
    std::shared_ptr<spdlog::logger> logger;
    LoggingConfig config;
    bool bootstrap{false};

    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    ~Pipeline() { pool.reset(); }  // The async logger only holds a weak_ptr: this joins the worker.

    [[nodiscard]] std::uint64_t overruns() const {
        return pool ? pool->overrun_counter() + pool->discard_counter() : 0;
    }
};

namespace {

spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace: return spdlog::level::trace;
        case LogLevel::debug: return spdlog::level::debug;
        case LogLevel::info: return spdlog::level::info;
        case LogLevel::warning: return spdlog::level::warn;
        case LogLevel::error: return spdlog::level::err;
        case LogLevel::critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

/// "%U": upper-case full level name (INFO, WARNING...) for the text format.
class UpperLevelFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
        const auto name = spdlog::level::to_string_view(msg.level);
        for (const char c : name) {
            dest.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<UpperLevelFlag>();
    }
};

std::unique_ptr<spdlog::formatter> make_formatter(LogFormat format) {
    auto formatter = std::make_unique<spdlog::pattern_formatter>();
    formatter->add_flag<UpperLevelFlag>('U');
    if (format == LogFormat::json) {
        formatter->set_pattern(R"({"ts":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l","pid":%P,"tid":%t,%v})");
    } else {
        formatter->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z %U pid=%P tid=%t %v");
    }
    return formatter;
}

}  // namespace

Logger::Logger() : level_(static_cast<int>(LogLevel::info)) { pipeline_.store(make_bootstrap()); }

Logger::~Logger() { shutdown(); }

std::shared_ptr<Logger::Pipeline> Logger::make_bootstrap() const {
    auto p = std::make_shared<Pipeline>();
    p->bootstrap = true;
    p->config.enabled = false;
    p->config.console = true;
    p->config.async.enabled = false;
    auto sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    p->logger = std::make_shared<spdlog::logger>("tsfs", sink);
    p->logger->set_formatter(make_formatter(LogFormat::text));
    p->logger->set_level(spdlog::level::trace);
    return p;
}

Result<void> Logger::configure(const LoggingConfig& config, bool force_console) {
    std::scoped_lock lock(configure_mutex_);
    auto p = std::make_shared<Pipeline>();
    p->config = config;
    std::vector<spdlog::sink_ptr> sinks;
    if (config.enabled) {
        if (config.rotation.compress) {
            p->compressor = std::make_shared<LogCompressor>(notices_);
        }
        RotatingFileSink::Options options{config.directory, config.filename, config.rotation};
        if (!config.rotation.enabled) {
            options.rotation.max_file_size = UINT64_MAX;
        }
        auto file = RotatingFileSink::create(std::move(options), notices_, p->compressor);
        if (!file) {
            return propagate(file.error());
        }
        p->file = *file;
        sinks.push_back(p->file);
    }
    if (config.console || force_console || !config.enabled) {
        auto console = std::make_shared<spdlog::sinks::stderr_sink_mt>();
        // With logging disabled, stderr still receives errors so that failures are never silent.
        console->set_level(config.enabled || force_console ? spdlog::level::trace : spdlog::level::err);
        sinks.push_back(console);
    }
    try {
        if (config.async.enabled) {
            p->pool = std::make_shared<spdlog::details::thread_pool>(
                config.async.queue_size, 1, [] { prepare_helper_thread("tsfs-log"); }, [] {});
            spdlog::async_overflow_policy policy = spdlog::async_overflow_policy::block;
            if (config.async.overflow_policy == LogOverflowPolicy::overrun_oldest) {
                policy = spdlog::async_overflow_policy::overrun_oldest;
            } else if (config.async.overflow_policy == LogOverflowPolicy::discard_new) {
                policy = spdlog::async_overflow_policy::discard_new;
            }
            p->logger = std::make_shared<spdlog::async_logger>("tsfs", sinks.begin(), sinks.end(), p->pool, policy);
        } else {
            p->logger = std::make_shared<spdlog::logger>("tsfs", sinks.begin(), sinks.end());
        }
        p->logger->set_formatter(make_formatter(config.format));
        p->logger->set_level(spdlog::level::trace);  // Filtering happens in Logger::log().
        p->logger->flush_on(spdlog::level::warn);
        // A sink exception must never propagate into a FUSE callback.
        p->logger->set_error_handler([](const std::string& msg) { stderr_fallback().report("logging error: " + msg); });
    } catch (const std::exception& e) {
        return make_error(ENOMEM, std::format("cannot initialize the logging pipeline: {}", e.what()));
    }

    auto previous = pipeline_.exchange(p);
    set_level(config.level);
    reconfigurations_.fetch_add(1);
    if (previous) {
        retired_overruns_.fetch_add(previous->overruns());
        if (previous->file) {
            retired_lost_.fetch_add(previous->file->lost_messages());
            retired_rotations_.fetch_add(previous->file->rotations());
        }
        try {
            previous->logger->flush();
        } catch (...) {
            stderr_fallback().report("cannot flush the previous logging pipeline");
        }
        // Dropped here (or by the last thread still using it): its queue is drained first.
    }
    return {};
}

void Logger::set_level(LogLevel level) noexcept { level_.store(static_cast<int>(level), std::memory_order_relaxed); }

void Logger::log(LogLevel level, std::string_view component, std::string_view event, std::string_view message,
                 std::initializer_list<LogField> fields, std::source_location location) {
    if (!enabled(level)) {
        return;
    }
    const auto p = pipeline_.load();
    if (!p) {
        return;
    }
    try {
        std::string body;
#ifdef TSFS_DEBUG_BUILD
        std::string_view file = location.file_name();
        if (const auto slash = file.rfind('/'); slash != std::string_view::npos) {
            file.remove_prefix(slash + 1);
        }
        const std::string src = std::format("{}:{}", file, location.line());
        std::vector<LogField> all(fields);
        all.emplace_back("src", src);
        body = render_event(p->config.format, component, event, message, all);
#else
        (void)location;
        body = render_event(p->config.format, component, event, message, std::span(fields.begin(), fields.size()));
#endif
        p->logger->log(spdlog::source_loc{}, to_spdlog(level), spdlog::string_view_t(body.data(), body.size()));
    } catch (const std::exception& e) {
        stderr_fallback().report(std::format("logging failed: {}", e.what()));
    }
}

void Logger::emit_notice(const LogNotice& notice) {
    if (notice.error_code != 0) {
        log(notice.level, "logging", notice.event, notice.message,
            {{"file", notice.path}, {"errno", notice.error_code}});
    } else {
        log(notice.level, "logging", notice.event, notice.message, {{"file", notice.path}});
    }
}

void Logger::maintenance() {
    for (const LogNotice& notice : notices_->drain()) {
        emit_notice(notice);
    }
    const auto p = pipeline_.load();
    if (!p) {
        return;
    }
    const std::uint64_t total = retired_overruns_.load() + p->overruns();
    const std::uint64_t reported = overruns_reported_.exchange(total);
    if (total > reported) {
        const std::uint64_t lost = total - reported;
        stderr_fallback().report(std::format("{} log message(s) dropped by the full logging queue", lost));
        log(LogLevel::warning, "logging", "log_queue_overflow", "the logging queue was full; messages were dropped",
            {{"dropped", lost}, {"policy", to_string(p->config.async.overflow_policy)}});
    }
}

void Logger::flush() {
    if (const auto p = pipeline_.load()) {
        try {
            p->logger->flush();
        } catch (...) {
            stderr_fallback().report("cannot flush the log");
        }
    }
}

void Logger::shutdown() {
    std::scoped_lock lock(configure_mutex_);
    maintenance();
    flush();
    auto previous = pipeline_.exchange(make_bootstrap());
    if (previous && previous->compressor) {
        previous->compressor->wait_idle();
    }
}

LoggerStats Logger::stats() const {
    LoggerStats s;
    s.reconfigurations = reconfigurations_.load();
    s.notices_dropped = notices_->dropped();
    s.queue_overruns = retired_overruns_.load();
    s.lost_writes = retired_lost_.load();
    s.rotations = retired_rotations_.load();
    if (const auto p = pipeline_.load()) {
        s.queue_overruns += p->overruns();
        if (p->file) {
            s.lost_writes += p->file->lost_messages();
            s.rotations += p->file->rotations();
        }
    }
    return s;
}

LoggingConfig Logger::active_config() const {
    const auto p = pipeline_.load();
    return p ? p->config : LoggingConfig{};
}

}  // namespace tsfs
