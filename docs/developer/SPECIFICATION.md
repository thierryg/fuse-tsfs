# PROJECT SPECIFICATION: TSFS
# Time-aware Storage Filesystem for Linux

You are a senior Linux systems engineer, filesystem architect, C++ performance
engineer, security engineer, and software quality specialist.

Your task is to design, implement, test, document, and deliver a production-grade
Linux userspace filesystem based on libfuse3.

The project name is TSFS (Time-aware Storage Filesystem).

Its primary purpose is to provide a mounted filesystem with configurable
file-retention policies and a high-performance garbage collector that
automatically removes files after their permitted lifetime.

This is a real software engineering project, not a prototype or a conceptual
demonstration.

You must implement working code, execute builds and tests, investigate failures,
fix defects, and produce complete documentation.

Do not stop after generating a design document or a collection of source files.
Continue until the project reaches a demonstrably functional state or a specific
environmental limitation prevents further progress.

All source code, identifiers, comments, documentation, log messages intended
for developers, and generated documentation must use technical US English.

===============================================================================
1. EXISTING PROJECT TO ANALYZE
===============================================================================

The reference project is:

    /home/tgayet/Documents/PERSO/remote-shutdown/

Before implementing anything:

1. Inspect the entire reference repository.
2. Read its README files and developer documentation.
3. Inspect its source tree, build system, CMake configuration, scripts,
   tests, installation mechanisms, coding conventions, and error handling.
4. Identify its design patterns and architectural conventions.
5. Examine how it handles logging, configuration, command-line arguments,
   signals, process lifecycle, installation, and uninstallation.
6. Identify reusable conventions without blindly copying inappropriate
   implementation details.
7. Determine which practices should be preserved in TSFS.
8. Produce an architecture and implementation plan consistent with the
   reference project's style.

Do not modify the reference repository.

Create the TSFS project in a separate directory unless the user explicitly
specifies another location.

Do not assume that the reference repository uses any particular technology.
Discover its actual implementation before making decisions.

If the reference repository cannot be accessed, report this limitation and
continue with a clearly documented, conventional Linux systems architecture.

===============================================================================
2. PROJECT OBJECTIVES
===============================================================================

TSFS must implement a real Linux filesystem using libfuse3.

The filesystem must enforce configurable retention policies for files.

Example requirements:

    *.mp4                   -> 1 day
    /tmp/record/            -> 1 day
    /tmp/record/*.mp4       -> 1 day
    /var/cache/tsfs/        -> 30 minutes
    /var/log/application/   -> 7 days
    *.core                  -> 2 days
    /mnt/tsfs/session-*/    -> 12 hours

The syntax above illustrates the intended capabilities. The actual configuration
grammar must be precisely defined, validated, documented, and tested.

The system must support:

- Global retention policies.
- Extension-based policies.
- Glob-based filename policies.
- Directory-based policies.
- Directory and filename combinations.
- Optional recursive directory matching.
- Per-policy expiration behavior.
- Configurable scan and cleanup intervals.
- Exclusion rules.
- Deterministic rule precedence.
- Concurrent filesystem operations.
- Efficient expiration scheduling.
- Runtime configuration reload.
- Crash diagnostics.
- Production and debugging builds.
- Extensive automated testing.
- Performance benchmarks.
- Code coverage reporting.
- Developer and user documentation.

The system must remain operational when the number of managed files becomes
large.

Design the architecture for millions of directory entries, subject to available
memory, storage performance, filesystem behavior, and the chosen indexing
strategy.

Do not claim scalability targets have been achieved unless they have been
measured.

===============================================================================
3. LANGUAGE AND TECHNOLOGY SELECTION
===============================================================================

Evaluate the most appropriate implementation language before writing the
production implementation.

At a minimum, evaluate:

A. Modern C++20 or C++23 with libfuse3.
B. Rust with a suitable FUSE implementation and bindings.

Compare the alternatives in terms of:

- Runtime performance.
- Filesystem callback overhead.
- Memory management.
- Concurrency.
- Lock contention.
- Allocation overhead.
- Integration with Linux system interfaces.
- libfuse3 compatibility.
- Error handling.
- Implementation complexity.
- Long-term maintainability.
- Debugging.
- Profiling.
- Testing.
- Availability of mature dependencies.

Use evidence and practical constraints rather than personal preference.

C++20 or C++23 is the default candidate because of its direct interoperability
with libfuse3, extensive Linux systems programming support, and mature tooling.

However, select Rust if the investigation demonstrates a material advantage
for this project's requirements.

Do not implement the same project twice.

Record the language decision in:

    docs/architecture/language-selection.md

The selected implementation must use the native facilities of its language
and must not introduce unnecessary abstraction overhead in performance-critical
paths.

For C++, prefer:

- RAII.
- Explicit ownership.
- std::chrono.
- std::filesystem where appropriate.
- std::jthread where appropriate.
- std::expected where supported by the selected standard.
- Strongly typed identifiers.
- Explicit error handling.
- Minimal dynamic allocation in hot paths.
- Immutable configuration snapshots.
- Carefully justified synchronization primitives.

Do not use raw pointers for ownership when safer alternatives are appropriate.

Avoid exceptions crossing C ABI boundaries.

All FUSE callbacks must translate internal errors into appropriate negative
errno values.

If Rust is selected, apply equivalent ownership, error handling, and
concurrency principles.

===============================================================================
4. FILESYSTEM ARCHITECTURE
===============================================================================

Implement TSFS as a modular userspace filesystem using libfuse3 or the
equivalent FUSE implementation justified by the language selection.

The architecture must separate:

1. Filesystem operations.
2. Retention policy evaluation.
3. Garbage collection.
4. File metadata indexing.
5. Configuration management.
6. Configuration reload.
7. Logging and observability.
8. Crash diagnostics.
9. Metrics.
10. Command-line handling.
11. Service lifecycle.
12. Persistent state, if required.
13. Testing and benchmarking.

Prefer an architecture in which the filesystem layer delegates to explicit
services rather than embedding business logic in FUSE callbacks.

Evaluate whether the filesystem should operate in:

A. Passthrough mode, exposing an existing directory tree through a mount.
B. Managed storage mode, maintaining files in a dedicated storage area.
C. Both modes, if this can be implemented without compromising safety.

Document the selected semantics.

For passthrough mode, define precisely which paths are visible through the
mount, how backing paths are mapped, and how path traversal is secured.

Do not expose arbitrary host paths accidentally.

The implementation must correctly handle, as applicable:

- getattr.
- readdir.
- open.
- create.
- read.
- write.
- flush.
- release.
- truncate.
- unlink.
- rename.
- mkdir.
- rmdir.
- statfs.
- access.
- fsync.
- fallocate, if supported.
- symlink and readlink, if supported.
- hard links, if supported.
- file permissions.
- ownership.
- timestamps.
- symbolic links.
- file descriptor lifecycle.

Explicitly document any unsupported operation.

Do not silently claim full POSIX filesystem compatibility.

Filesystem operations must remain correct while the garbage collector is running.

===============================================================================
5. RETENTION POLICY ENGINE
===============================================================================

Design a dedicated retention policy engine.

A policy should support at least:

- A unique policy identifier.
- A matching scope.
- A matching pattern.
- A retention duration.
- An enabled or disabled state.
- An explicit priority, if appropriate.
- An optional exclusion mechanism.
- An optional recursive matching flag.
- Optional creation-time or modification-time expiration semantics.

Define a typed representation for policies.

Example conceptual configuration:

    retention:
      default: "7d"

      rules:
        - id: temporary-recordings
          path: "/tmp/record"
          recursive: true
          retention: "1d"

        - id: mp4-files
          glob: "*.mp4"
          retention: "1d"

        - id: application-cache
          path: "/var/cache/tsfs"
          recursive: true
          retention: "30m"

        - id: core-dumps
          glob: "*.core"
          retention: "2d"

The exact YAML grammar must be finalized before implementation.

Define how these concepts differ:

- An exact path.
- A directory prefix.
- A basename glob.
- A path glob.
- A recursive directory rule.
- An extension rule.
- A global default.

A glob must not be interpreted as a regular expression unless explicitly
specified.

Avoid ambiguous matching behavior.

Define a deterministic precedence model. A reasonable starting point is:

1. Explicit exclusions.
2. Higher explicit priority.
3. More specific matching scope.
4. More specific pattern.
5. Stable declaration order as a final tie-breaker.

Review whether exclusions should always override every inclusion rule.

Document the final behavior.

The policy engine must provide:

- Efficient rule lookup.
- Deterministic evaluation.
- Validated duration parsing.
- Overflow-safe expiration calculations.
- Unit tests for all matching combinations.
- Tests for conflicting rules.
- Tests for malformed paths.
- Tests for invalid patterns.
- Tests for empty or missing policies.

Do not compile or evaluate every rule from scratch for every filesystem
operation.

Cache compiled matchers and optimize the matching process where benchmarks
justify the complexity.

===============================================================================
6. FILE LIFETIME SEMANTICS
===============================================================================

Define the exact meaning of file lifetime.

Support configurable expiration based on at least one of:

- Time since file creation or first observation.
- Time since last modification.
- An explicitly defined retention timestamp, if available.

Creation time is not universally available or reliable on Linux filesystems.
Do not treat ctime as creation time: ctime normally represents inode status
change time.

If creation time is selected, document how unavailable or unreliable birth
times are handled.

The system must account for:

- Files being written while expiration is approaching.
- Open file descriptors.
- Files modified during GC processing.
- Rename operations.
- Atomic replacement of files.
- Concurrent unlink operations.
- Files disappearing during a scan.
- Files with changed inode numbers.
- System clock changes.
- System restarts.
- Incomplete metadata.
- Filesystem permission errors.

A file must not be deleted based on a stale metadata record.

Before destructive operations, verify that the candidate still corresponds
to the file that was evaluated.

Where applicable, use inode and device identity, descriptor-relative operations,
and other race-resistant Linux interfaces.

Do not blindly follow symbolic links.

Document the effects of hard links, mount boundaries, and external modifications.

An open file that has been unlinked may continue consuming storage until its
last reference is released. Explain and measure this behavior.

The system must never silently delete files outside its configured managed
scope.

===============================================================================
7. HIGH-PERFORMANCE GARBAGE COLLECTOR
===============================================================================

The garbage collector is the primary performance-critical component.

Design it independently from the FUSE request-processing path.

Do not perform a full filesystem scan for every file operation.

Evaluate and implement an architecture combining:

- A metadata index.
- An expiration queue.
- A dedicated background worker pool.
- A bounded work queue.
- Periodic reconciliation scans.
- Efficient scheduling of the next expiration.
- Concurrent-safe metadata updates.
- Incremental index maintenance.
- Optional persistent metadata.
- Recovery after an unclean shutdown.

Use an expiration-oriented priority queue, timing wheel, or another scheduling
structure only after evaluating the expected workload.

Compare at least:

A. Binary heap or priority queue.
B. Timing wheel.
C. Indexed ordered structure.
D. Hierarchical scheduling or another appropriate strategy.

Document the chosen approach in:

    docs/architecture/garbage-collector.md

Consider a min-heap keyed by expiration time as the baseline implementation.

Avoid excessive heap operations and unbounded queue growth.

The implementation must handle updates and cancellation efficiently.

Evaluate lazy invalidation, generation counters, indexed heap updates, or other
appropriate approaches.

The GC must not block normal filesystem operations while scanning or deleting
large numbers of files.

Use configurable limits for:

- Maximum deletion operations per second.
- Maximum concurrent deletion workers.
- Maximum pending GC work items.
- Maximum scan duration per iteration.
- Scan interval.
- Retry count.
- Retry delay.
- Maximum retry backoff.
- Maximum permitted batch size.

Make these limits explicit and documented.

A filesystem with millions of files must not require millions of simultaneous
in-memory tasks.

Implement backpressure.

The GC must support:

- Expired file detection.
- Candidate scheduling.
- Candidate validation.
- Safe deletion.
- Error classification.
- Retry handling.
- Cancellation.
- Statistics.
- Graceful shutdown.
- Recovery and reconciliation.

Avoid excessive CPU usage when no files are due to expire.

Use condition variables, event-driven wakeups, or equivalent mechanisms
instead of continuously polling at very short intervals.

Separate discovery from deletion.

Do not execute expensive directory traversal synchronously inside a FUSE
callback.

Provide a dry-run mode that reports which files would be deleted without
actually deleting them.

Dry-run mode must use the same policy evaluation and candidate validation
logic as real garbage collection.

Deletion must be idempotent where possible.

Handle ENOENT, EACCES, EBUSY, EIO, and other relevant errors explicitly.

Do not automatically retry permanent failures forever.

Use bounded retries and exponential backoff with configurable limits.

Provide an optional maximum number of deletions per GC cycle.

Design metrics that make GC effectiveness and bottlenecks measurable.

===============================================================================
8. METADATA INDEX AND PERSISTENCE
===============================================================================

Evaluate the appropriate metadata storage strategy.

Possible approaches include:

- In-memory metadata.
- On-disk metadata journal.
- SQLite or another embedded database.
- A lightweight append-only journal.
- Rebuilding metadata through filesystem reconciliation.

Do not add a database without demonstrating its value.

Compare the costs of:

- Startup recovery.
- Metadata durability.
- Random reads.
- Sequential writes.
- Lock contention.
- Write amplification.
- Memory consumption.
- Corruption recovery.
- Operational complexity.

Document the selected approach.

If persistent state is implemented, define transaction and recovery semantics.

The metadata index must never become the sole authority for whether a file exists
or whether a destructive operation is still valid.

Verify relevant filesystem state before deletion.

If the index is missing, stale, or corrupt, the system must fail safely and
recover through a documented procedure.

Do not allow corrupted metadata to trigger broad, unintended deletion.

===============================================================================
9. CONFIGURATION: /etc/tsfs/config.yml
===============================================================================

The default configuration file must be:

    /etc/tsfs/config.yml

Also support an explicit configuration path through the command line.

For example:

    tsfs --config /path/to/config.yml

Use a mature YAML parser compatible with the selected implementation language.

Validate the complete configuration before activating it.

Reject:

- Invalid YAML.
- Unknown mandatory values.
- Invalid duration strings.
- Invalid paths.
- Invalid patterns.
- Invalid worker counts.
- Negative or overflowing durations.
- Invalid queue limits.
- Unsupported configuration versions.
- Inconsistent settings.

Provide clear errors containing the file location and relevant parameter.

Do not partially activate a configuration that failed validation.

Implement runtime configuration reload.

Support SIGHUP and automatic detection of configuration file changes.

Use inotify or another appropriate Linux mechanism for detecting changes.

Account for atomic file replacement, which can invalidate a watch on the original
inode. Re-establish watches as necessary.

A failed reload must preserve the last valid configuration.

Apply configuration changes atomically by building a new immutable configuration
snapshot and publishing it only after validation succeeds.

Long-running operations must use a consistent configuration snapshot.

Define the behavior when a policy changes while a file is already scheduled
for expiration.

The implementation must recalculate or invalidate affected scheduling entries
without allowing stale entries to trigger incorrect deletion.

Configuration reload must not require restarting the filesystem.

Avoid holding global locks while parsing YAML or performing expensive
configuration validation.

Create a comprehensive commented example configuration:

    config/config.example.yml

Document every supported option.

Provide a generated or maintained configuration reference in:

    docs/user/configuration.md

===============================================================================
10. CONFIGURATION EXAMPLE
===============================================================================

Use the following conceptual configuration as a starting point.

Do not assume the field names are final.

    version: 1

    filesystem:
      mountpoint: "/mnt/tsfs"
      backing_directory: "/var/lib/tsfs/data"
      mode: "passthrough"
      allow_other: false
      read_only: false

    retention:
      default: "7d"
      evaluation_timestamp: "last_modified"

      rules:
        - id: temporary-recordings
          path: "/tmp/record"
          recursive: true
          retention: "1d"
          enabled: true

        - id: video-files
          glob: "*.mp4"
          retention: "1d"
          enabled: true

        - id: temporary-cache
          path: "/var/cache/tsfs"
          recursive: true
          retention: "30m"
          enabled: true

    garbage_collector:
      enabled: true
      scan_interval: "60s"
      max_workers: 4
      max_deletions_per_second: 100
      max_pending_tasks: 10000
      batch_size: 100
      dry_run: false
      retry_limit: 5
      retry_initial_delay: "1s"
      retry_max_delay: "60s"

    metadata:
      backend: "memory"
      persistent_state: false

    logging:
      level: "info"
      file: "/var/log/tsfs/tsfs.log"

    diagnostics:
      enabled: false
      core_dumps: false
      stack_trace_on_crash: false

    metrics:
      enabled: true

    security:
      refuse_symlink_escape: true
      stay_on_filesystem: true

Validate all settings against the implemented schema.

The example must not contain options that are silently ignored.

If a field is unsupported, either implement it or remove it and document the
decision.

Never make a potentially destructive retention configuration the only default
configuration.

===============================================================================
11. CRASH DIAGNOSTICS AND CORE DUMPS
===============================================================================

Provide a debug diagnostics subsystem with two independent diagnostic modes.

Mode A: Text stack trace.

On a supported fatal signal, generate a diagnostic report containing, as
available:

- Signal name and number.
- Process ID.
- Thread ID.
- Timestamp.
- Fault address.
- Instruction pointer.
- Loaded module information.
- Symbolized stack frames.
- Relevant diagnostic context.

Use an appropriate native stack-trace mechanism.

For C++, evaluate execinfo, libunwind, and platform-specific facilities.

The diagnostic implementation must account for async-signal-safety.

Do not call arbitrary C++ code, acquire ordinary mutexes, allocate memory,
or perform unsafe logging from a fatal signal handler.

A minimal signal handler may capture essential information and use
async-signal-safe operations to notify a separate diagnostic mechanism.

If symbolization requires external tools or additional processing, perform it
outside the fatal signal handler.

Mode B: Native core dump.

Enable native Linux core dumps when explicitly requested and supported by the
execution environment.

Document the required kernel settings, RLIMIT_CORE, core_pattern, permissions,
storage requirements, and systemd-coredump integration.

Do not assume that setting ulimit -c is sufficient on every Linux system.

Provide instructions for analyzing core dumps with GDB.

Both diagnostic modes must be independently configurable.

Also allow both modes to be enabled simultaneously.

Example:

    diagnostics:
      enabled: true
      text_stack_trace: true
      native_core_dump: true

Support configuration through both YAML and appropriate command-line options.

Implement a crash diagnostic helper or subsystem with clear ownership and
lifecycle.

Test diagnostics using a dedicated subprocess or isolated test executable.

Do not deliberately crash the main test runner.

Verify the resulting diagnostic artifacts.

Do not attempt to recover execution after undefined behavior or memory corruption.

Fatal-signal handling must not interfere with normal process termination.

Document limitations for SIGKILL, stack overflow, and other situations where
complete diagnostic capture cannot be guaranteed.

===============================================================================
12. DEBUG AND RELEASE BUILDS
===============================================================================

Provide separate build configurations.

Release:

- Optimized compilation.
- Appropriate warning levels.
- Production-oriented logging defaults.
- No unnecessary diagnostic overhead.
- Debug symbols optionally available through a separate symbol package or
  an explicitly documented configuration.

Debug:

- Debug symbols.
- Reduced optimization.
- Extensive assertions where appropriate.
- Additional diagnostics.
- Sanitizer support where compatible.
- Stack traces.
- Configurable native core dumps.
- Debug logging.

Provide optional AddressSanitizer and UndefinedBehaviorSanitizer builds.

Evaluate ThreadSanitizer separately because it may require different build flags
and can conflict with other sanitizers.

Document sanitizer limitations and incompatibilities.

Use CMAKE_BUILD_TYPE or a multi-configuration build generator appropriately.

The exact compiler flags must be compatible with the selected compiler.

Do not enable sanitizers indiscriminately in production builds.

Provide practical command examples for all supported build modes.

===============================================================================
13. PROJECT STRUCTURE
===============================================================================

Create a clean and maintainable project layout.

Use the following as an initial proposal, adapting it to the conventions
discovered in the reference repository.

    tsfs/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── README.md
    ├── LICENSE
    ├── .gitignore
    ├── .clang-format
    ├── .clang-tidy
    ├── CMake/
    │   ├── Dependencies.cmake
    │   ├── CompilerOptions.cmake
    │   ├── Sanitizers.cmake
    │   ├── Coverage.cmake
    │   ├── Documentation.cmake
    │   └── Packaging.cmake
    ├── config/
    │   └── config.example.yml
    ├── include/
    │   └── tsfs/
    ├── src/
    │   ├── main.cpp
    │   ├── filesystem/
    │   ├── policy/
    │   ├── gc/
    │   ├── metadata/
    │   ├── config/
    │   ├── diagnostics/
    │   ├── logging/
    │   ├── metrics/
    │   └── cli/
    ├── tests/
    │   ├── unit/
    │   ├── integration/
    │   ├── filesystem/
    │   ├── gc/
    │   ├── config/
    │   ├── diagnostics/
    │   ├── stress/
    │   └── performance/
    ├── benchmarks/
    ├── scripts/
    ├── systemd/
    ├── packaging/
    └── docs/
        ├── architecture/
        ├── developer/
        ├── user/
        ├── security/
        ├── testing/
        └── performance/

Do not create empty directories merely to imitate this tree.

Each directory must have a real purpose.

===============================================================================
14. DESIGN PATTERNS
===============================================================================

Use design patterns where they solve actual architectural problems.

Do not apply design patterns mechanically.

Consider these patterns:

Strategy:
- Retention evaluation.
- Expiration scheduling.
- Metadata backend selection.
- Diagnostic output.

Factory:
- Metadata backend construction.
- Filesystem backend selection.
- Scheduler construction when multiple implementations are supported.

Observer or event-driven architecture:
- Configuration changes.
- Filesystem metadata updates.
- GC scheduling notifications.

Repository:
- Metadata access and persistence.

Command:
- GC work items.
- Administrative operations, where appropriate.

RAII:
- File descriptors.
- Locks.
- Temporary resources.
- Mount lifecycle.
- Signal and diagnostic resources where applicable.

Dependency injection:
- Policy engine dependencies.
- Metadata repositories.
- Clock abstraction.
- Filesystem operation interfaces.

State machine:
- File lifecycle and GC candidate processing.

Use immutable configuration snapshots to simplify safe runtime reload.

Avoid unnecessary singleton usage, global mutable state, deeply nested
inheritance, and excessive virtual dispatch.

Prefer composition over inheritance.

Keep interfaces small and focused.

Document each pattern that is actually used and explain why it improves
maintainability, testability, or performance.

Provide an architecture diagram and class diagrams.

===============================================================================
15. CMAKE BUILD SYSTEM
===============================================================================

The primary build entry point must be:

    CMakeLists.txt

Use a supported CMake version and declare the minimum version explicitly.

The user mentioned CMakefile.txt, but CMake conventionally uses CMakeLists.txt.
Provide CMakeLists.txt as the canonical build file.

If a CMakefile.txt compatibility wrapper is genuinely useful, document its
purpose rather than duplicating the build configuration.

The build system must expose the following targets:

    deps
    install
    uninstall
    build
    tests
    doc

The targets must be documented and usable.

Important:

CMake's standard `install` target is generated automatically. Integrate with
it instead of creating conflicting or ambiguous install targets.

The `build` target may be provided as an explicit convenience target, but the
standard CMake build command must also work.

Implement `uninstall` safely, preferably using an install manifest generated
by CMake.

The uninstall process must not delete files that TSFS did not install.

The `deps` target must verify required dependencies for the selected build,
test, documentation, coverage, and benchmark features.

At a minimum, evaluate:

- CMake.
- A supported C++ compiler, if C++ is selected.
- pkg-config.
- libfuse3 development files.
- A YAML parser.
- A logging library, if used.
- A unit-testing framework.
- Google Benchmark or an alternative.
- Doxygen.
- Graphviz.
- gcov.
- lcov.
- genhtml.
- GDB.
- Relevant sanitizer support.
- Required Linux development headers.

Use pkg-config or CMake package discovery for dependency detection.

Do not assume that every optional tool is mandatory for a normal release build.

Clearly distinguish:

- Required production dependencies.
- Required test dependencies.
- Required documentation dependencies.
- Optional diagnostics and profiling dependencies.

The `deps` target must report missing dependencies clearly and return a
nonzero exit code when dependencies required for the requested operation
are missing.

Avoid silently downloading dependencies or modifying the operating system.

Document package installation commands for Ubuntu 24.04 and other supported
distributions.

Do not run privileged installation commands without explicit authorization.

===============================================================================
16. TESTING FRAMEWORK
===============================================================================

Implement comprehensive automated tests.

Use a mature unit-testing framework appropriate to the selected language.

Separate tests into:

A. Unit tests.
B. Filesystem integration tests.
C. Policy evaluation tests.
D. Configuration tests.
E. Runtime reload tests.
F. Garbage collector tests.
G. Concurrency tests.
H. Crash diagnostics tests.
I. Security tests.
J. Stress tests.
K. Performance benchmarks.
L. Regression tests.

Unit tests must not require root privileges unless unavoidable.

Integration tests requiring FUSE mounts must detect unavailable capabilities
and report a clear skip or failure according to the test configuration.

Do not silently mark skipped tests as passed.

Use temporary directories and isolated test fixtures.

Tests must not modify real user files or delete data outside their temporary
storage areas.

Use an injectable clock for deterministic expiration tests.

Avoid tests that rely on sleeping for long durations.

Provide a controllable clock and scheduler where appropriate.

Test at least the following:

Policy matching:
- Global default.
- Extension matching.
- Basename globs.
- Path globs.
- Exact directories.
- Recursive directories.
- Combined directory and extension policies.
- Rule precedence.
- Exclusions.
- Invalid patterns.
- Empty configurations.

Expiration:
- File not yet expired.
- File exactly at expiration.
- Expired file.
- Modified file.
- Renamed file.
- Replaced file.
- Missing file.
- File modified concurrently with GC.
- Open file during GC.
- Clock changes.
- Restart and recovery.

Garbage collection:
- Empty storage.
- One expired file.
- Large numbers of expired files.
- Mixed expired and valid files.
- Worker shutdown.
- Queue saturation.
- Retry handling.
- Permission failures.
- Concurrent deletion attempts.
- Stale metadata entries.
- Configuration reload during scheduling.
- Dry-run operation.
- Bounded deletion rate.
- Backpressure.

Configuration:
- Valid YAML.
- Invalid YAML.
- Missing file.
- Invalid duration.
- Invalid policy.
- Atomic replacement.
- Reload success.
- Reload failure.
- Preservation of the previous configuration.
- Multiple consecutive reloads.

Filesystem:
- File creation.
- Reading.
- Writing.
- Truncation.
- Rename.
- Unlink.
- Directory operations.
- Permissions.
- Large files.
- Concurrent access.
- Symbolic-link handling.
- Hard-link handling, if supported.

Security:
- Path traversal.
- Symlink escape.
- Mount-boundary enforcement.
- Invalid paths.
- Unauthorized destructive operations.
- Stale metadata.
- Unexpected backing filesystem changes.

Concurrency:
- Simultaneous create and delete.
- Simultaneous write and GC.
- Concurrent configuration reload.
- Multiple GC workers.
- Queue pressure.
- Shutdown while tasks are pending.

Every discovered defect must produce a regression test when practical.

===============================================================================
17. GCOV AND LCOV CODE COVERAGE
===============================================================================

Use gcov and lcov to generate coverage reports.

Generate an HTML report using genhtml.

The coverage process must:

1. Configure a dedicated coverage build.
2. Enable appropriate coverage compiler flags.
3. Build the instrumented code.
4. Reset previous coverage counters.
5. Execute the relevant tests.
6. Capture coverage data.
7. Remove irrelevant system and generated files from the report.
8. Generate HTML output.
9. Produce a machine-readable summary where possible.
10. Report uncovered files and lines.

Store generated artifacts under a predictable directory, such as:

    build/coverage/

Use a separate coverage build directory so that coverage flags do not leak into
normal release builds.

Document compiler and lcov version compatibility.

Provide a documented way to exclude test scaffolding, generated code, and
third-party dependencies.

Do not exclude production code merely to improve coverage percentages.

Establish initial coverage measurements and then introduce realistic thresholds
after measuring the existing project.

Aim for:

- High unit-test coverage.
- Meaningful branch coverage for retention decisions.
- Full coverage of critical policy precedence cases.
- Strong coverage of destructive-operation safeguards.
- Coverage of configuration failure paths.
- Coverage of GC retry and recovery behavior.

Do not claim that coverage proves the absence of defects.

Do not treat unexecuted branches as tested.

Explain how to interpret the HTML report.

Use coverage information to identify potentially dead code, but do not claim
that gcov or lcov alone can reliably identify all dead code.

Complement coverage with static analysis and review of unreachable or unused
implementation paths.

===============================================================================
18. CRASH DUMPS DURING TESTING
===============================================================================

Provide a dedicated test mode that enables core dumps and crash diagnostics
where the operating system permits it.

Requirements:

- Check and report RLIMIT_CORE.
- Record the effective core-dump configuration.
- Check the relevant core_pattern configuration when accessible.
- Support systemd-coredump where present.
- Use isolated subprocesses for deliberate crash tests.
- Preserve diagnostic artifacts when a test fails.
- Record stack traces when available.
- Avoid polluting the source tree.
- Document artifact locations.
- Ensure ordinary test failures do not terminate the entire test suite.
- Never deliberately crash the main test runner.

Support optional environment controls such as:

    TSFS_TEST_ENABLE_CORE_DUMPS=1
    TSFS_TEST_ARTIFACT_DIR=build/test-artifacts

The exact variable names may be adjusted, but they must be documented.

The tests target must report when core-dump generation is unavailable.

Do not attempt to modify global kernel configuration automatically.

===============================================================================
19. PERFORMANCE BENCHMARKS
===============================================================================

Implement a dedicated benchmark suite.

Measure filesystem performance independently from GC performance.

Filesystem benchmarks must include, where applicable:

- File creation throughput.
- Sequential read throughput.
- Sequential write throughput.
- Small-file creation.
- Small-file deletion.
- Metadata lookup latency.
- Directory enumeration.
- Rename throughput.
- Concurrent client operations.
- Operations with GC disabled.
- Operations with GC enabled.

GC benchmarks must include:

- Time to discover expired files.
- Time to schedule expiration candidates.
- Time to delete expired files.
- Throughput in files deleted per second.
- CPU consumption.
- Memory consumption.
- Queue depth.
- Scheduler overhead.
- Configuration reload overhead.
- Startup and recovery time.

Use representative datasets such as:

- 1,000 files.
- 100,000 files.
- 1,000,000 files, when resources permit.
- Mixed file sizes.
- Different expiration distributions.
- Many files in a single directory.
- Many directories containing relatively few files.

Large-scale benchmarks must be opt-in if they require substantial storage,
time, or memory.

Clearly distinguish synthetic benchmarks from real-world measurements.

Record:

- CPU model.
- Memory capacity.
- Kernel version.
- Compiler version.
- Build type.
- libfuse version.
- Storage device.
- Backing filesystem.
- Dataset size.
- Benchmark parameters.
- Warm-cache and cold-cache conditions where meaningful.

Do not fabricate performance numbers.

Provide repeatable benchmark commands.

Compare different scheduler implementations where useful.

Measure lock contention and allocation overhead when they appear significant.

Establish baseline results before introducing optimization claims.

Prefer profiling evidence over speculative micro-optimizations.

===============================================================================
20. OBSERVABILITY AND ADMINISTRATION
===============================================================================

Provide structured logging and useful runtime statistics.

Consider the following counters and gauges:

- Total managed files.
- Number of scheduled expirations.
- Expired candidates.
- Successful deletions.
- Failed deletions.
- Retry count.
- Pending GC tasks.
- GC queue saturation events.
- Last successful reconciliation.
- Current configuration generation.
- Configuration reload failures.
- Worker utilization.
- Files skipped because they were modified or replaced.
- Total bytes deleted.
- Time spent scanning.
- Time spent deleting.

Avoid maintaining inaccurate counters in highly contended global atomics.

Use per-worker counters or periodic aggregation where beneficial.

Provide an administrative interface, such as a command-line status command,
if it can be implemented safely.

Example conceptual commands:

    tsfs --check-config
    tsfs --dry-run
    tsfs --show-config
    tsfs --version
    tsfs --help

Consider a status command that communicates with a running daemon.

Do not invent a complex IPC protocol without a concrete requirement.

Any IPC endpoint must have documented authentication and permission rules.

Never expose administrative deletion commands to untrusted users.

===============================================================================
21. SECURITY REQUIREMENTS
===============================================================================

Treat deletion as a security-sensitive operation.

The system must implement defense in depth.

Requirements include:

- Explicitly defined managed storage boundaries.
- Safe path resolution.
- Symlink escape prevention.
- Appropriate descriptor-relative filesystem operations.
- Validation of destructive-operation candidates.
- Least-privilege execution.
- Safe handling of configuration files.
- Correct permissions for logs and state files.
- Safe handling of temporary files.
- No shell interpolation of untrusted paths.
- No unsafe command construction.
- Bounded resource consumption.
- Input validation.
- Race-condition analysis.
- Clear error handling.
- Safe shutdown.

If possible, use openat2 with suitable resolution restrictions on supported
Linux kernels, while providing a documented fallback where necessary.

Do not rely solely on string-prefix checks to establish that a path is inside
a managed directory.

Define how mount boundaries and bind mounts are handled.

If the filesystem supports passthrough mode, distinguish the mount namespace
visible to the process from the host's filesystem namespace.

Do not promise security guarantees that the selected implementation cannot
enforce.

Create:

    docs/security/threat-model.md

Include assets, trust boundaries, attackers, attack surfaces, mitigations,
residual risks, and security testing procedures.

===============================================================================
22. USER DOCUMENTATION
===============================================================================

Write comprehensive user documentation in technical US English.

The user documentation must include:

1. Introduction.
2. Supported platforms.
3. System requirements.
4. Installation.
5. Dependency installation.
6. Building from source.
7. Release installation.
8. Debug installation.
9. Configuration reference.
10. Retention policy examples.
11. Mounting the filesystem.
12. Unmounting the filesystem.
13. Runtime configuration reload.
14. Garbage collector behavior.
15. Dry-run mode.
16. Logging.
17. Monitoring.
18. Performance tuning.
19. Crash diagnostics.
20. Core-dump analysis.
21. Troubleshooting.
22. Uninstallation.
23. Security recommendations.
24. Known limitations.

Provide complete practical examples.

For example, document how to configure:

- Temporary video recordings with a one-day lifetime.
- All MP4 files with a one-day lifetime.
- A temporary cache with a thirty-minute lifetime.
- Application logs with a seven-day lifetime.
- Core dumps with a two-day lifetime.
- A global retention policy.
- A directory-specific exception.
- A dry-run cleanup.
- A runtime policy update.

Explain rule precedence and expiration semantics using explicit examples.

Document whether hidden files match glob rules and how directory recursion works.

===============================================================================
23. DEVELOPER DOCUMENTATION
===============================================================================

Provide comprehensive developer documentation.

At a minimum:

    docs/developer/building.md
    docs/developer/coding-style.md
    docs/developer/testing.md
    docs/developer/debugging.md
    docs/developer/coverage.md
    docs/developer/performance.md
    docs/developer/contributing.md

Also provide:

    docs/architecture/overview.md
    docs/architecture/filesystem.md
    docs/architecture/policy-engine.md
    docs/architecture/garbage-collector.md
    docs/architecture/configuration-reload.md
    docs/architecture/concurrency.md
    docs/architecture/metadata.md
    docs/security/threat-model.md
    docs/testing/test-strategy.md

Document:

- Component responsibilities.
- Class responsibilities.
- Ownership and lifetime rules.
- Threading model.
- Synchronization strategy.
- Error propagation.
- Policy matching algorithms.
- Scheduler data structures.
- Configuration reload.
- Persistence and recovery.
- Filesystem operation semantics.
- Crash handling.
- Extension points.
- Performance trade-offs.

Keep documentation synchronized with the implementation.

Do not document hypothetical behavior as implemented functionality.

===============================================================================
24. SOURCE CODE DOCUMENTATION
===============================================================================

Document the code thoroughly but avoid meaningless comments.

Every public class, public interface, public function, and important public
data structure must have appropriate documentation.

For C++, use Doxygen-compatible documentation.

Document:

- Purpose.
- Responsibilities.
- Parameters.
- Return values.
- Exceptions, if applicable.
- Error behavior.
- Ownership.
- Thread-safety guarantees.
- Synchronization requirements.
- Preconditions.
- Postconditions.
- Performance considerations when important.
- Relevant invariants.

Private implementation details must be documented where understanding the
algorithm or concurrency behavior requires it.

Pay special attention to:

- Destructive operations.
- File identity validation.
- Expiration calculations.
- Synchronization.
- Lock ordering.
- Atomic configuration replacement.
- Signal handling.
- Resource ownership.
- Retry logic.
- Persistent metadata.
- Recovery procedures.

Do not add comments that simply repeat the code.

Use meaningful names and small, focused functions.

===============================================================================
25. DOXYGEN DOCUMENTATION
===============================================================================

The doc target must generate HTML documentation using Doxygen.

Integrate Graphviz where available for diagrams.

Generate documentation under:

    build/docs/html/

Include:

- Public API documentation.
- Class hierarchy.
- Include dependencies.
- Call graphs where practical.
- Class diagrams.
- Architecture documentation.
- Module descriptions.
- Configuration reference links.

Provide a suitable Doxyfile or generate one through CMake.

Fail or warn appropriately when required documentation tools are missing.

The documentation build must be reproducible.

Fix broken references and significant Doxygen warnings.

Do not suppress all warnings merely to obtain a successful build.

===============================================================================
26. INSTALLATION AND UNINSTALLATION
===============================================================================

Implement safe installation through CMake.

Use GNUInstallDirs where appropriate.

Install the necessary artifacts, including:

- The TSFS executable.
- Public headers only if required.
- Example configuration.
- Documentation.
- Man pages if provided.
- Systemd unit files if provided.
- Supporting resources.

Do not overwrite an existing user configuration without explicit authorization.

Provide an example configuration separately from the active configuration.

Use appropriate filesystem permissions.

The install target must not require root privileges merely to build the project.

Explain when elevated privileges are needed for installation.

The uninstall target must use the install manifest or another safe mechanism.

Do not delete:

- User data.
- The managed storage directory.
- Runtime files not owned by TSFS.
- Modified configuration files without explicit confirmation.

Document service shutdown and unmounting before removal.

===============================================================================
27. CONTINUOUS INTEGRATION AND STATIC ANALYSIS
===============================================================================

Provide a repeatable local quality-assurance workflow.

Integrate the following where practical:

- Compiler warnings.
- clang-format.
- clang-tidy.
- CTest.
- AddressSanitizer.
- UndefinedBehaviorSanitizer.
- ThreadSanitizer.
- gcov/lcov.
- Doxygen.
- Static analysis.
- Dependency checks.
- Regression tests.

Use a strict but maintainable warning policy.

Treat serious compiler warnings as defects.

Do not introduce broad warning suppressions.

If a warning must be suppressed, document the reason and limit the suppression
to the smallest possible scope.

Provide an optional CI configuration if the repository's conventions support
it.

Do not introduce an unnecessary CI platform dependency.

===============================================================================
28. COMMAND-LINE INTERFACE
===============================================================================

Implement a documented CLI.

The exact interface may evolve, but it should provide commands or options
equivalent to:

    tsfs --help
    tsfs --version
    tsfs --config /etc/tsfs/config.yml
    tsfs --check-config
    tsfs --dry-run
    tsfs --debug
    tsfs --foreground

Provide suitable options for:

- Configuration path.
- Mount point.
- Backing directory, where applicable.
- Logging level.
- Foreground execution.
- Configuration validation.
- Dry-run GC.
- Diagnostic options.
- Version information.

Avoid ambiguous option interactions.

Provide clear usage errors.

The help output must match the actual implementation.

Do not expose command-line flags that do nothing.

If a separate administrative CLI is needed, document the distinction.

===============================================================================
29. IMPLEMENTATION WORKFLOW
===============================================================================

Work incrementally and keep the project buildable.

PHASE 1: REPOSITORY ANALYSIS

Inspect the reference repository.

Document its conventions and reusable practices.

PHASE 2: REQUIREMENTS AND ARCHITECTURE

Write:

    docs/architecture/overview.md
    docs/architecture/language-selection.md
    docs/architecture/garbage-collector.md
    docs/architecture/filesystem.md
    docs/security/threat-model.md

Identify unresolved requirements and state explicit assumptions.

Do not spend excessive time producing speculative documentation.

PHASE 3: BUILD FOUNDATION

Create the CMake build system.

Implement dependency detection, compiler options, tests, and documentation
targets.

Ensure that a minimal executable builds successfully.

PHASE 4: POLICY ENGINE

Implement configuration parsing, validation, matching, precedence, and
deterministic unit tests.

PHASE 5: FILESYSTEM

Implement the selected FUSE architecture.

Validate basic filesystem behavior independently of garbage collection.

PHASE 6: GARBAGE COLLECTOR

Implement the scheduler, metadata index, worker pool, candidate validation,
safe deletion, retry handling, and metrics.

PHASE 7: HOT RELOAD

Implement atomic configuration replacement and scheduler reconciliation.

Test malformed configuration and concurrent reload.

PHASE 8: CRASH DIAGNOSTICS

Implement text stack traces and native core-dump support.

Validate them using dedicated subprocesses.

PHASE 9: QUALITY AND PERFORMANCE

Implement the complete test suite, coverage reporting, stress tests, and
benchmarks.

Fix discovered defects.

PHASE 10: DOCUMENTATION

Generate Doxygen HTML and finish the user and developer documentation.

PHASE 11: RELEASE VALIDATION

Run the complete release validation workflow.

Inspect installation, uninstallation, recovery, error handling, and security.

Produce a final implementation report.

At the end of each phase:

1. Build the project.
2. Run the relevant tests.
3. Fix failures.
4. Record any limitations.
5. Update the documentation.
6. Continue to the next phase.

Do not mark a phase complete merely because source files have been generated.

===============================================================================
30. REQUIRED CMAKE TARGETS AND USAGE
===============================================================================

All commands below must be documented and tested.

Dependency checks:

    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release --target deps

Release build:

    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release --target build -j"$(nproc)"

Debug build:

    cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build/debug --target build -j"$(nproc)"

Run tests:

    cmake --build build/debug --target tests

Coverage build:

    cmake -S . -B build/coverage \
        -DCMAKE_BUILD_TYPE=Debug \
        -DTSFS_ENABLE_COVERAGE=ON

    cmake --build build/coverage -j"$(nproc)"

    cmake --build build/coverage --target tests

    cmake --build build/coverage --target coverage

Documentation:

    cmake --build build/release --target doc

Installation:

    sudo cmake --install build/release

Uninstallation:

    sudo cmake --build build/release --target uninstall

The exact syntax may be adjusted to fit the implemented build system.

For example, the build target may depend on the default CMake build behavior,
and the tests target may invoke CTest.

However, every required target must have a defined and useful purpose.

Ensure that the documented coverage workflow actually captures data and
generates HTML.

Do not merely define empty custom targets.

Avoid requiring sudo for commands that do not need elevated privileges.

Document any limitations associated with uninstallation after the build
directory has been removed.

===============================================================================
31. ACCEPTANCE CRITERIA
===============================================================================

The project is acceptable only when the following conditions are satisfied
or explicitly documented as blocked by environmental constraints.

BUILD:

- The project configures successfully.
- Dependencies are detected correctly.
- Release builds work.
- Debug builds work.
- The compiler reports no unexplained serious warnings.
- The required CMake targets exist and work.

FILESYSTEM:

- The FUSE implementation mounts successfully in a supported environment.
- Basic file operations work.
- Permissions are handled correctly.
- Unsupported operations are documented.
- Destructive operations respect the configured scope.

RETENTION:

- All implemented policy types work.
- Policy precedence is deterministic.
- Expiration semantics are documented.
- Modified and replaced files are handled safely.
- Configuration reload works without restarting the mount.

GARBAGE COLLECTION:

- Expired files are discovered and deleted.
- Valid files are preserved.
- Queue growth is bounded.
- Work is processed asynchronously.
- Retry behavior is bounded.
- GC can be disabled.
- Dry-run works.
- Reconciliation recovers from stale metadata.

DIAGNOSTICS:

- Text stack traces can be generated in supported environments.
- Native core dumps can be enabled when permitted by the environment.
- Crash tests run in isolated subprocesses.
- Diagnostic limitations are documented.

TESTING:

- Unit tests pass.
- Integration tests pass where the required environment is available.
- Concurrency tests are provided.
- Security tests are provided.
- Regression tests cover discovered defects.
- Performance benchmarks are reproducible.

COVERAGE:

- gcov instrumentation works.
- lcov captures coverage.
- genhtml generates a usable HTML report.
- Coverage artifacts are stored predictably.

DOCUMENTATION:

- Doxygen HTML is generated.
- User documentation is complete.
- Developer documentation is complete.
- Configuration options are documented.
- Release and debug workflows are documented.
- Security limitations are documented.

INSTALLATION:

- Installation works.
- Uninstallation works safely.
- Existing configuration is not overwritten unexpectedly.
- Managed user data is preserved.

Do not claim acceptance criteria are met without executing the relevant checks.

===============================================================================
32. ENGINEERING CONSTRAINTS
===============================================================================

Follow these principles:

- KISS.
- SOLID where applicable.
- Separation of concerns.
- Explicit ownership.
- Fail-safe defaults.
- Least privilege.
- Deterministic behavior.
- Bounded resource usage.
- Minimal hot-path overhead.
- Testability.
- Maintainability.
- Measurable performance.
- Clear error reporting.
- Reproducible builds.
- Complete operational documentation.

Avoid:

- Premature optimization.
- Unnecessary dependencies.
- Unnecessary abstraction layers.
- Unbounded queues.
- Full-tree scans on every operation.
- Busy-wait loops.
- Global locks held during filesystem traversal.
- Silent configuration failures.
- Unsafe deletion.
- Signal handlers that perform complex unsafe operations.
- Hard-coded paths outside documented defaults.
- Shell commands constructed from untrusted input.
- Fake implementations.
- Placeholder methods in production paths.
- Empty tests that always succeed.
- Performance claims without measurements.
- Documentation that does not match the code.

Prefer simple, robust algorithms before introducing sophisticated
optimizations.

Measure before optimizing.

===============================================================================
33. FINAL DELIVERABLES
===============================================================================

Deliver a complete repository containing:

1. Production source code.
2. Public interfaces where appropriate.
3. CMake build configuration.
4. Dependency checks.
5. Example YAML configuration.
6. Runtime configuration reload.
7. FUSE filesystem implementation.
8. Retention policy engine.
9. Garbage collector.
10. Metadata management.
11. Crash diagnostic support.
12. Unit tests.
13. Integration tests.
14. Security tests.
15. Concurrency tests.
16. Stress tests.
17. Performance benchmarks.
18. gcov/lcov coverage integration.
19. Doxygen configuration.
20. Generated HTML documentation.
21. User documentation.
22. Developer documentation.
23. Security documentation.
24. Installation support.
25. Uninstallation support.
26. Debug and release build instructions.
27. A final test and benchmark report.

At the end, provide a concise implementation report containing:

- Selected language and rationale.
- Final architecture.
- Implemented features.
- Important design decisions.
- Build results.
- Test results.
- Coverage summary.
- Benchmark results, if executed.
- Generated documentation location.
- Installation instructions.
- Known limitations.
- Remaining work.

Clearly distinguish between:

- Implemented and tested.
- Implemented but not tested.
- Partially implemented.
- Not implemented.
- Blocked by the environment.

Never invent successful command outputs, coverage percentages, benchmark results,
or test results.

===============================================================================
34. START NOW
===============================================================================

Begin by inspecting:

    /home/tgayet/Documents/PERSO/remote-shutdown/

Then determine the current working directory and establish the TSFS project
location.

Report the discovered repository conventions and the proposed implementation
architecture.

After this initial analysis, proceed with implementation.

Do not ask for confirmation about routine engineering decisions.

Make reasonable decisions, document assumptions, and continue autonomously.

Ask questions only when an unresolved decision could materially affect data
safety, filesystem semantics, security, or an irreversible operation.

Never delete or modify user data during development.

Do not perform privileged system modifications without explicit authorization.

Prioritize correctness and safe deletion before performance optimization.

The final objective is a maintainable, documented, testable, high-performance
Linux filesystem with reliable file-retention policies and a measurable,
efficient garbage collector.

===============================================================================
35. LOGGING SYSTEM AND SIZE-BASED LOG ROTATION
===============================================================================

Implement a production-grade logging subsystem for TSFS.

The logging subsystem must be designed for a high-performance Linux filesystem
where logging must not significantly impact filesystem operations or garbage
collector performance.

All logging configuration must be managed through:

    /etc/tsfs/config.yml

The default log directory must be:

    /var/log/tsfs/

The default active log file must be:

    /var/log/tsfs/tsfs.log

The logging subsystem must support configurable log levels, structured log
messages, asynchronous logging where appropriate, and automatic log rotation
based on file size.

All logging behavior must be documented in technical US English.

-------------------------------------------------------------------------------
35.1. LOGGING REQUIREMENTS
-------------------------------------------------------------------------------

Implement the following logging levels:

- trace
- debug
- info
- warning
- error
- critical

The minimum enabled logging level must be configurable at runtime.

The default production logging level should be info.

The default debug-build logging level should be debug, unless explicitly
overridden by the configuration.

Log messages must contain, where applicable:

- Timestamp with millisecond precision.
- Severity.
- Process ID.
- Thread ID.
- Component name.
- Source location in debug builds, where useful.
- Event identifier, where appropriate.
- Relevant error code.
- Human-readable message.

Use a consistent and documented format.

Consider a format such as:

    2026-10-13T10:15:30.123+02:00 INFO pid=1234 tid=1235 component=gc event=file_deleted path="/tmp/record/video.mp4" bytes=1048576

The example is illustrative. Implement a format that is practical to parse
and maintain.

Support at least:

- Human-readable text logging.
- Structured logging suitable for machine processing, preferably JSON Lines,
  if this can be implemented without excessive complexity.

Avoid logging sensitive file contents or unnecessary sensitive information.

Define how pathnames containing whitespace, control characters, invalid UTF-8,
or embedded special characters are represented.

Use a mature logging library when it offers clear benefits.

Evaluate libraries such as spdlog for a C++ implementation.

Do not implement an unnecessarily complex custom logging framework if a mature
library provides the required features.

-------------------------------------------------------------------------------
35.2. DEFAULT LOG DIRECTORY AND FILES
-------------------------------------------------------------------------------

Use the following default layout:

    /var/log/tsfs/
    ├── tsfs.log
    ├── tsfs.log.1
    ├── tsfs.log.2
    ├── tsfs.log.3
    └── ...

The exact number of retained files must be configurable.

The active log file is:

    /var/log/tsfs/tsfs.log

Rotated files must have deterministic names and a documented ordering.

Use a configurable file naming convention if useful.

The logging subsystem must not create arbitrary files outside the configured
log directory.

The log directory must be created during installation or service initialization
when appropriate.

Directory creation must use appropriate ownership and permissions.

Do not automatically assume that the process runs as root.

Document the required permissions when TSFS runs under a dedicated service
account.

Never make log files world-writable.

If the directory cannot be created or opened, report the error through stderr
or another safe fallback and follow a documented failure policy.

Do not silently disable logging.

-------------------------------------------------------------------------------
35.3. SIZE-BASED ROTATION
-------------------------------------------------------------------------------

Implement automatic log rotation triggered by the size of the active log file.

The maximum active log file size must be configurable.

Example configuration:

    logging:
      enabled: true
      directory: "/var/log/tsfs"
      filename: "tsfs.log"
      level: "info"
      format: "text"

      rotation:
        enabled: true
        max_file_size: "50MiB"
        max_files: 10
        check_interval: "1s"
        compress: false

The values above are defaults proposed for implementation and may be adjusted
after evaluating the expected workload.

The implementation must define the precise semantics of max_file_size and
max_files.

For example, with max_files set to 10, the intended retained set is:

    tsfs.log
    tsfs.log.1
    tsfs.log.2
    ...
    tsfs.log.10

In this interpretation, the active file is not counted among the rotated files.

Document this behavior explicitly.

The rotation algorithm must:

1. Detect when the active log file reaches or exceeds the configured limit.
2. Prevent concurrent rotation attempts from corrupting the log sequence.
3. Safely close or synchronize the active file as required.
4. Rotate older files in descending order.
5. Move the active file to the first rotated position.
6. Open a new active log file.
7. Preserve the configured permissions and ownership.
8. Remove rotated files beyond the retention limit.
9. Resume logging without restarting the filesystem.
10. Report rotation failures safely.

Rotation must not silently lose log messages.

The implementation must consider the following failure cases:

- The log directory becomes unavailable.
- The active log file is deleted externally.
- The filesystem runs out of space.
- The process lacks permission to rename files.
- A rotated file cannot be removed.
- The active log file is replaced externally.
- Rotation fails while several threads are logging.
- The process restarts after an incomplete rotation.
- The configuration changes during rotation.

Use a mature library's rotation support where appropriate.

If using spdlog, evaluate its rotating file sink and asynchronous logging facilities.

Do not add a second rotation mechanism that conflicts with the logging library.

Do not depend on external logrotate configuration for the primary size-based
rotation mechanism.

An optional logrotate integration may be provided only if its interaction
with the application's own rotation mechanism is explicitly defined and tested.

-------------------------------------------------------------------------------
35.4. ROTATION GRANULARITY AND OVERSHOOT
-------------------------------------------------------------------------------

Define how closely the implementation enforces the configured maximum size.

A periodic file-size check can allow the file to exceed the configured limit
between checks.

Prefer an approach that checks size at an appropriate point in the write path
or uses a logging library with well-defined rotation semantics.

Avoid performing an expensive stat() syscall for every log message unless
measurements demonstrate that the overhead is acceptable.

If using asynchronous logging, account for queued messages and concurrent
writers when determining the rotation threshold.

Document whether max_file_size is a strict upper bound or an approximate
rotation threshold.

Use appropriate synchronization so that concurrent writers cannot independently
trigger overlapping rotations.

Rotation must not introduce unbounded lock contention.

-------------------------------------------------------------------------------
35.5. OPTIONAL LOG COMPRESSION
-------------------------------------------------------------------------------

Support optional compression of rotated log files if it can be implemented
reliably.

For example:

    logging:
      rotation:
        compress: true
        compression_level: 3

Evaluate whether compression should occur:

- Synchronously during rotation.
- Asynchronously in a background worker.
- After a rotated file has been closed.

Prefer asynchronous compression for performance-sensitive workloads.

Compression must never block FUSE callbacks.

The compression subsystem must use a bounded work queue and must not consume
unlimited memory or CPU.

A rotated file must not be deleted before its compressed replacement has been
successfully written and validated.

Use an atomic replacement procedure where appropriate.

If compression fails, preserve the original uncompressed log whenever possible.

Do not compress the active log file.

Document the compression format and any required dependencies.

Compression may be disabled by default.

-------------------------------------------------------------------------------
35.6. LOGGING PERFORMANCE
-------------------------------------------------------------------------------

Logging must not become a bottleneck for filesystem operations or garbage
collection.

Evaluate asynchronous logging using a dedicated logging thread or a mature
asynchronous logging library.

Define a bounded logging queue.

Make the queue capacity configurable.

Example:

    logging:
      async:
        enabled: true
        queue_size: 8192
        overflow_policy: "block"

Supported overflow policies must be explicitly defined.

Evaluate:

- block
- overrun_oldest
- discard_new

Choose a safe default and document its behavior.

For production audit-relevant events, do not silently discard messages without
making the loss observable.

For high-volume debug or trace messages, controlled dropping may be acceptable
if explicitly configured.

Ensure that the logging implementation cannot deadlock because the logging
thread attempts to log its own internal failures.

Do not recursively invoke the ordinary logger from inside the logger's
failure-handling path.

Measure the performance impact of:

- Synchronous logging.
- Asynchronous logging.
- Debug logging enabled.
- Debug logging disabled.
- Rotation under sustained load.
- Concurrent filesystem operations.
- GC running concurrently with filesystem activity.

Provide benchmarks or measurements where practical.

Avoid logging every successful read or write operation by default.

High-frequency operations must use appropriately configurable trace-level
logging.

-------------------------------------------------------------------------------
35.7. LOG EVENTS
-------------------------------------------------------------------------------

Use consistent event identifiers for important events.

At a minimum, provide events for:

Configuration:

- config_loaded
- config_reload_started
- config_reload_succeeded
- config_reload_failed

Filesystem:

- filesystem_mount_started
- filesystem_mounted
- filesystem_unmounted
- filesystem_operation_failed

Garbage collection:

- gc_started
- gc_completed
- gc_candidate_found
- gc_file_deleted
- gc_file_skipped
- gc_delete_failed
- gc_retry_scheduled
- gc_queue_saturated
- gc_reconciliation_started
- gc_reconciliation_completed

Logging:

- log_rotation_started
- log_rotation_completed
- log_rotation_failed
- log_compression_started
- log_compression_completed
- log_compression_failed
- log_queue_overflow
- log_write_failed

Diagnostics:

- diagnostic_report_started
- diagnostic_report_completed
- diagnostic_report_failed

Do not log sensitive data unnecessarily.

Ensure that a single failure does not generate an unbounded stream of identical
error messages.

Implement rate limiting or aggregation for repetitive failures where
appropriate.

Provide periodic summaries for recurring GC failures instead of emitting
unlimited identical messages.

-------------------------------------------------------------------------------
35.8. CRASH-SAFE LOGGING
-------------------------------------------------------------------------------

Normal logging and fatal-signal diagnostics must be separate subsystems.

Do not assume that an asynchronous logger remains usable after a fatal signal.

Do not acquire ordinary logging locks in a fatal signal handler.

Do not allocate memory or perform complex formatting from an unsafe signal
handler.

The crash diagnostic mechanism must use a documented, safe strategy.

It may write a minimal diagnostic record through an appropriate low-level
mechanism or notify a dedicated diagnostic process.

Do not guarantee complete logging after arbitrary memory corruption.

Document the limitations.

Ensure that native core-dump generation remains independent of ordinary log
rotation.

Crash reports and core dumps must not be deleted by the normal log rotation
mechanism.

-------------------------------------------------------------------------------
35.9. RUNTIME CONFIGURATION RELOAD
-------------------------------------------------------------------------------

The logging configuration must be reloadable without restarting TSFS.

Example:

    logging:
      enabled: true
      directory: "/var/log/tsfs"
      filename: "tsfs.log"
      level: "debug"
      format: "json"

      rotation:
        enabled: true
        max_file_size: "100MiB"
        max_files: 20
        check_interval: "1s"
        compress: false

Changing the following parameters must take effect after a successful reload:

- enabled
- level
- format
- directory
- filename
- rotation.enabled
- rotation.max_file_size
- rotation.max_files
- rotation.check_interval
- rotation.compress
- rotation.compression_level
- async.enabled
- async.queue_size
- async.overflow_policy

Only expose settings that are actually implemented.

Changing the log directory or filename must use a controlled sink replacement
procedure.

The previous logging sink must remain usable until the new sink has been
successfully initialized.

A failed logging configuration reload must preserve the previous working
logging configuration.

Do not allow logging reconfiguration to interrupt filesystem operations
unnecessarily.

Define how queued messages are handled when switching logging sinks.

Avoid losing queued messages during a normal logging reconfiguration.

Do not introduce deadlocks between the configuration manager and logging
subsystem.

Test repeated and concurrent configuration reloads.

-------------------------------------------------------------------------------
35.10. LOGGING CONFIGURATION REFERENCE
-------------------------------------------------------------------------------

Extend the main configuration reference with a complete description of all
logging settings.

Document:

- Default value.
- Allowed values.
- Unit of measurement.
- Validation rules.
- Runtime reload behavior.
- Performance implications.
- Failure behavior.
- Security considerations.

Use explicit units for sizes.

Support a documented set of size suffixes, for example:

    B
    KiB
    MiB
    GiB

If decimal units such as MB are supported, distinguish them clearly from
binary units.

Reject malformed or overflowing values.

Do not silently interpret an invalid size as zero.

Provide configuration examples for:

A. Minimal production logging.
B. Debug logging.
C. JSON logging.
D. Small files with frequent rotation.
E. Large files with infrequent rotation.
F. Asynchronous logging.
G. Optional compression.
H. Logging disabled.
I. Runtime logging level changes.

-------------------------------------------------------------------------------
35.11. LOGGING TESTS
-------------------------------------------------------------------------------

Add unit, integration, stress, and performance tests for the logging subsystem.

Test at least:

- Creation of the default log directory.
- Creation of the active log file.
- Every supported log level.
- Invalid logging configuration.
- Rotation at the configured threshold.
- Rotation with multiple files.
- Deletion of files beyond max_files.
- Concurrent writes from many threads.
- Concurrent rotation requests.
- Configuration reload.
- Log directory change.
- Log filename change.
- Rotation disabled.
- Logging disabled.
- Queue saturation.
- Each supported overflow policy.
- Disk-full simulation where feasible.
- Permission failures.
- External deletion of the active file.
- Restart after incomplete rotation.
- Compression success and failure, if supported.
- Failure-path logging without recursive failure.
- Logger shutdown with queued messages.
- Logging during sustained GC activity.
- Logging under high filesystem operation rates.

Use temporary directories.

Never use the real /var/log/tsfs directory in ordinary unit tests.

Do not require root privileges for basic logging tests.

Performance tests must measure throughput, latency, and resource consumption.

Test that logging rotation does not corrupt files or deadlock concurrent
writers.

Verify that retained files match the configured rotation policy.

===============================================================================
35.12. INSTALLATION AND OPERATIONS
===============================================================================

Installation must create or prepare:

    /var/log/tsfs/

only when appropriate and with suitable permissions.

Do not overwrite existing log files.

Document how to configure ownership for a dedicated TSFS service account.

Provide a systemd integration example if the project includes a systemd unit.

Document the interaction between:

- Application-managed rotation.
- systemd service lifecycle.
- systemd-coredump.
- External log collection agents.
- Optional log shipping.

The application's native rotation mechanism must be the primary owner of
rotated TSFS log files.

If external log rotation is supported, document how to avoid conflicts.

Provide operational guidance for:

- Inspecting the active log.
- Listing rotated logs.
- Following logs in real time.
- Changing the log level.
- Testing rotation.
- Diagnosing rotation failures.
- Recovering from a full log filesystem.
- Adjusting the maximum size and retained file count.

Example commands may include:

    tail -F /var/log/tsfs/tsfs.log

    ls -lh /var/log/tsfs/

    du -sh /var/log/tsfs/

Document the commands and their expected purpose.

===============================================================================
35.13. ACCEPTANCE CRITERIA FOR LOGGING
===============================================================================

The logging subsystem is acceptable only when:

- Logs are written under /var/log/tsfs/ by default.
- The active log filename is configurable.
- The logging level is configurable.
- Rotation is based on file size.
- The retained file count is configurable.
- Rotation works with concurrent writers.
- The implementation has a documented rotation threshold policy.
- Configuration reload works without restarting TSFS.
- Invalid configuration does not destroy the previous working logger.
- Rotation errors are reported safely.
- The logging subsystem uses bounded resources.
- High-frequency logging overhead has been measured.
- Crash diagnostics do not depend on ordinary asynchronous logging.
- Unit and integration tests cover rotation.
- All logging configuration options are documented.
- The installation process respects ownership and permissions.
- The implementation does not silently discard important diagnostic events.

Add the logging subsystem to the project architecture, build system,
test suite, performance benchmarks, Doxygen documentation, user manual,
developer manual, and final acceptance report.

Do not consider logging complete until the relevant tests have been executed.

Configuration YAML recommandée

Pour que Claude Code dispose d'une référence immédiatement exploitable, tu peux également lui demander d'intégrer cette configuration à config/config.example.yml :

logging:
  enabled: true

  directory: "/var/log/tsfs"
  filename: "tsfs.log"

  level: "info"
  format: "text"

  async:
    enabled: true
    queue_size: 8192
    overflow_policy: "block"

  rotation:
    enabled: true

    # Maximum size of the active log file.
    max_file_size: "50MiB"

    # Number of rotated files retained, excluding the active file.
    max_files: 10

    # Maximum interval between periodic rotation checks.
    check_interval: "1s"

    compress: false
    compression_level: 3

Avec ces paramètres, le système conservera le fichier actif et jusqu'à dix fichiers tournants, soit onze fichiers au maximum, hors fichiers temporaires de rotation ou de compression.

La limite de 50MiB doit être considérée comme un seuil de rotation dont le dépassement éventuel dépendra de l'implémentation retenue. Il faudra le préciser dans la documentation.

Trois décisions techniques à imposer à Claude Code
Rotation native de l'application : utiliser de préférence un composant éprouvé, tel que le rotating sink de spdlog si C++ est retenu, plutôt que de réinventer le mécanisme.
Séparation des journaux et des dumps : les core dumps et les rapports de crash doivent disposer de leur propre politique de conservation. Ils ne doivent jamais être supprimés par la rotation ordinaire des logs.
Gestion des erreurs : si le disque est plein ou si le journal devient inaccessible, TSFS doit signaler le problème sans provoquer une boucle infinie de journalisation ni bloquer indéfiniment les opérations du filesystem.

Cette extension s'intègre au prompt maître précédent et rend la journalisation, la rotation et leur validation explicites dans les exigences du projet.
