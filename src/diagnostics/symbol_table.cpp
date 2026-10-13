// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/diagnostics/symbol_table.cpp
// Purpose : ELF symbol table loading (normal context) and signal-safe lookup
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "diagnostics/symbol_table.hpp"

#include <cxxabi.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "common/unique_fd.hpp"

namespace tsfs::diagnostics {

namespace {

constexpr std::size_t kMaxSectionBytes = 512ULL << 20;  // Refuse absurd section sizes.
constexpr std::size_t kMaxNameLength = 512;             // Long template names are truncated.

bool read_at(int fd, void* buffer, std::size_t size, std::uint64_t offset) {
    auto* out = static_cast<char*>(buffer);
    while (size > 0) {
        const ssize_t n = ::pread(fd, out, size, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        out += n;
        size -= static_cast<std::size_t>(n);
        offset += static_cast<std::uint64_t>(n);
    }
    return true;
}

std::string demangle(const char* name) {
    int status = -1;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string out = status == 0 && demangled != nullptr ? demangled : name;
    std::free(demangled);  // NOLINT(cppcoreguidelines-no-malloc): __cxa_demangle's contract.
    if (out.size() > kMaxNameLength) {
        out.resize(kMaxNameLength - 3);
        out += "...";
    }
    return out;
}

}  // namespace

/// Collects the symbols of one object after another (normal context only).
struct SymbolTableBuilder {
    SymbolTable table;

    std::uint32_t add_text(const std::string& s) {
        const auto offset = static_cast<std::uint32_t>(table.text_.size());
        table.text_.append(s);
        table.text_.push_back('\0');
        return offset;
    }

    /// Read the function symbols of the file @p open_path, loaded with @p bias and
    /// reported as @p label. @return false if skipped.
    bool add_object(const std::string& open_path, const std::string& label, std::uintptr_t bias) {
        const UniqueFd fd(::open(open_path.c_str(), O_RDONLY | O_CLOEXEC));
        if (!fd) {
            return false;
        }
        Elf64_Ehdr eh{};
        if (!read_at(fd.get(), &eh, sizeof eh, 0) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
            eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0) {
            return false;
        }
        std::vector<Elf64_Shdr> sections(eh.e_shnum);
        if (!read_at(fd.get(), sections.data(), sections.size() * sizeof(Elf64_Shdr), eh.e_shoff)) {
            return false;
        }
        // The static table also lists internal-linkage functions; .dynsym is the fallback
        // of a stripped object.
        const Elf64_Shdr* symtab = nullptr;
        for (const Elf64_Word type : {Elf64_Word{SHT_SYMTAB}, Elf64_Word{SHT_DYNSYM}}) {
            const auto it = std::ranges::find_if(sections, [&](const Elf64_Shdr& s) { return s.sh_type == type; });
            if (it != sections.end()) {
                symtab = &*it;
                break;
            }
        }
        if (symtab == nullptr || symtab->sh_link >= sections.size() || symtab->sh_entsize != sizeof(Elf64_Sym) ||
            symtab->sh_size > kMaxSectionBytes || sections[symtab->sh_link].sh_size > kMaxSectionBytes) {
            return false;
        }
        const Elf64_Shdr& strtab = sections[symtab->sh_link];
        std::vector<Elf64_Sym> symbols(symtab->sh_size / sizeof(Elf64_Sym));
        std::string names(strtab.sh_size, '\0');
        if (!read_at(fd.get(), symbols.data(), symbols.size() * sizeof(Elf64_Sym), symtab->sh_offset) ||
            !read_at(fd.get(), names.data(), names.size(), strtab.sh_offset)) {
            return false;
        }
        const std::uint32_t module = add_text(label);
        for (const Elf64_Sym& sym : symbols) {
            const unsigned type = ELF64_ST_TYPE(sym.st_info);
            if ((type != STT_FUNC && type != STT_GNU_IFUNC) || sym.st_shndx == SHN_UNDEF || sym.st_value == 0 ||
                sym.st_size == 0 || sym.st_name >= names.size()) {
                continue;
            }
            const std::uintptr_t start = bias + sym.st_value;
            table.entries_.push_back(
                {start, start + sym.st_size, add_text(demangle(names.c_str() + sym.st_name)), module});
        }
        return true;
    }
};

std::unique_ptr<const SymbolTable> SymbolTable::load_current_process() {
    struct Object {
        std::string path;
        std::uintptr_t bias;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> loads;  ///< PT_LOAD [start, end).
    };
    std::vector<Object> objects;
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void* data) {
            Object object{info->dlpi_name != nullptr ? info->dlpi_name : "", info->dlpi_addr, {}};
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr) & ph = info->dlpi_phdr[i];
                if (ph.p_type == PT_LOAD) {
                    const std::uintptr_t start = info->dlpi_addr + ph.p_vaddr;
                    object.loads.emplace_back(start, start + ph.p_memsz);
                }
            }
            static_cast<std::vector<Object>*>(data)->push_back(std::move(object));
            return 0;
        },
        &objects);
    SymbolTableBuilder builder;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const Object& object = objects[i];
        std::string open_path = object.path;
        std::string label = object.path;
        if (i == 0 && object.path.empty()) {
            // The executable (empty name): /proc/self/exe is the running inode even if
            // the file was replaced on disk; it is reported under its real path.
            open_path = "/proc/self/exe";
            char exe[4096];
            const ssize_t n = ::readlink(open_path.c_str(), exe, sizeof exe - 1);
            label = n > 0 ? std::string(exe, static_cast<std::size_t>(n)) : open_path;
        } else if (object.path.empty() || object.path.starts_with("linux-vdso")) {
            continue;  // vDSO: no file behind it.
        }
        if (builder.add_object(open_path, label, object.bias)) {
            ++builder.table.objects_loaded_;
        } else {
            ++builder.table.objects_skipped_;
        }
        const std::uint32_t module = builder.add_text(label);
        for (const auto& [start, end] : object.loads) {
            builder.table.segments_.push_back({start, end, object.bias, module});
        }
    }
    auto& entries = builder.table.entries_;
    std::ranges::sort(entries, {}, &Entry::start);
    std::ranges::sort(builder.table.segments_, {}, &Segment::start);
    if (builder.table.text_.size() > std::numeric_limits<std::uint32_t>::max()) {
        entries.clear();  // Offsets would not fit: degrade to "no symbols" rather than lie.
        builder.table.segments_.clear();
    }
    builder.table.text_.shrink_to_fit();
    return std::make_unique<const SymbolTable>(std::move(builder.table));
}

bool SymbolTable::lookup(std::uintptr_t address, Match& match) const noexcept {
    // Last entry whose start <= address (manual binary search: nothing that could
    // allocate or throw in a signal handler).
    std::size_t lo = 0;
    std::size_t hi = entries_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (entries_[mid].start <= address) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    // Aliases and nested symbols share or overlap ranges: look back a few entries.
    for (std::size_t i = lo, checked = 0; i > 0 && checked < 8; --i, ++checked) {
        const Entry& e = entries_[i - 1];
        if (address >= e.start && address < e.end) {
            match.function = text_.data() + e.name;
            match.offset = address - e.start;
            match.module = text_.data() + e.module;
            return true;
        }
    }
    return false;
}

bool SymbolTable::locate(std::uintptr_t address, const char*& module, std::uintptr_t& offset) const noexcept {
    std::size_t lo = 0;
    std::size_t hi = segments_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (segments_[mid].start <= address) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || address >= segments_[lo - 1].end) {
        return false;
    }
    const Segment& segment = segments_[lo - 1];
    module = text_.data() + segment.module;
    offset = address - segment.bias;
    return true;
}

}  // namespace tsfs::diagnostics
