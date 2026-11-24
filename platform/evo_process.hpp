// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Runs a command without a shell, with bounded time and output.
#ifndef ENTRY_AAD_PLATFORM_EVO_PROCESS_HPP
#define ENTRY_AAD_PLATFORM_EVO_PROCESS_HPP
#include <string>
#include <vector>
namespace evo {
// Diagnostic copy only: partial/failed text must never be used by the normal scene/AP parsers.
struct CommandReport {
    std::string text;
    int exitCode, signal, spawnError;
    bool timedOut, truncated;
    CommandReport() : exitCode(-1), signal(0), spawnError(0), timedOut(false), truncated(false) {}
};
// No shell; bounded output/time; reap the child on every exit path.
// Uses posix_spawnp: safe to call while the AA/D-Bus worker threads are alive. environment: the child's whole
// environment (NULL-terminated), or nullptr for ours.
bool runCommand(const std::string& executable, const std::vector<std::string>& args, std::string& output,
                int timeoutMs = 2000, CommandReport* diagnostic = nullptr, char* const* environment = nullptr);
}  // namespace evo
#endif
