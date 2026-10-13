// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/main.cpp
// Purpose : Program entry point: parse the command line and run the selected action
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <exception>
#include <print>

#include "app/application.hpp"
#include "cli/command_line.hpp"

// std::println cannot throw here (constant, valid format string): the analyzer does
// not know that and reports the catch handler.
int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
    try {
        const auto options = tsfs::parse_command_line(argc, argv);
        if (!options) {
            std::println(stderr, "tsfs: {}", options.error().message);
            return tsfs::kExitUsage;
        }
        return tsfs::run_application(*options);
    } catch (const std::exception& e) {
        std::println(stderr, "tsfs: fatal error: {}", e.what());
        return tsfs::kExitRuntime;
    }
}
