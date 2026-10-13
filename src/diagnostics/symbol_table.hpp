// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/diagnostics/symbol_table.hpp
// Purpose : Function symbols of the loaded ELF objects, resolvable from a signal handler
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tsfs::diagnostics {

/**
 * @brief Immutable address -> function name table of the current process.
 *
 * Built once, in a normal context, by load_current_process(): every ELF object
 * loaded at that time (the executable and its shared libraries, enumerated with
 * dl_iterate_phdr) is read from disk; the function symbols of its static symbol
 * table (.symtab, which also holds internal-linkage functions) or, when the file is
 * stripped, of its dynamic symbol table (.dynsym) are relocated by the load bias,
 * demangled and sorted.
 *
 * lookup() only reads that memory (binary search, no allocation, no lock, no
 * system call), so it is async-signal-safe: the crash handler uses it to print
 * function names at crash time. Objects loaded later (dlopen) are not covered;
 * file and line numbers need the debug information (scripts/tsfs-symbolize.sh).
 *
 * Thread-safety: immutable after construction; lookup() may run concurrently.
 */
class SymbolTable {
public:
    /// A resolved address.
    struct Match {
        const char* function{nullptr};  ///< Demangled name (NUL-terminated, owned by the table).
        std::uintptr_t offset{0};       ///< address - function start.
        const char* module{nullptr};    ///< Path of the ELF object (NUL-terminated, owned by the table).
    };

    /// Load the symbols of every object currently loaded. Never throws for an
    /// unreadable or unsupported object: it is skipped (see objects_skipped()).
    static std::unique_ptr<const SymbolTable> load_current_process();

    /**
     * @brief Find the function containing @p address.
     * @return true and fill @p match when a function symbol covers the address.
     * Async-signal-safe.
     */
    bool lookup(std::uintptr_t address, Match& match) const noexcept;

    /**
     * @brief Find the loaded object containing @p address (when no symbol covers it).
     * @param address    Code address to locate.
     * @param[out] module Path of the object. @param[out] offset Address relative to
     *             the object's load bias (what addr2line expects).
     * @return true when a loaded segment contains the address. Async-signal-safe.
     */
    bool locate(std::uintptr_t address, const char*& module, std::uintptr_t& offset) const noexcept;

    /// @return the number of function symbols.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    /// @return the number of objects whose symbols could be read.
    [[nodiscard]] std::size_t objects_loaded() const noexcept { return objects_loaded_; }
    /// @return the number of objects skipped (unreadable, not ELF64, no symbol table).
    [[nodiscard]] std::size_t objects_skipped() const noexcept { return objects_skipped_; }

private:
    struct Entry {
        std::uintptr_t start;
        std::uintptr_t end;
        std::uint32_t name;    ///< Offset of the function name in text_.
        std::uint32_t module;  ///< Offset of the module path in text_.
    };
    friend struct SymbolTableBuilder;

    SymbolTable() = default;

    struct Segment {
        std::uintptr_t start;
        std::uintptr_t end;
        std::uintptr_t bias;
        std::uint32_t module;  ///< Offset of the module path in text_.
    };

    std::vector<Entry> entries_;     ///< Sorted by start.
    std::vector<Segment> segments_;  ///< PT_LOAD segments of every object, sorted by start.
    std::string text_;               ///< NUL-separated names and module paths.
    std::size_t objects_loaded_{0};
    std::size_t objects_skipped_{0};
};

}  // namespace tsfs::diagnostics
