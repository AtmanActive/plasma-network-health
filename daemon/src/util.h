// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <sys/types.h>
#include <string>

namespace nh {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void logSetLevel(LogLevel level);
LogLevel logLevel();
void logPrintf(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define NH_ERROR(...) ::nh::logPrintf(::nh::LogLevel::Error, __VA_ARGS__)
#define NH_WARN(...) ::nh::logPrintf(::nh::LogLevel::Warn, __VA_ARGS__)
#define NH_INFO(...) ::nh::logPrintf(::nh::LogLevel::Info, __VA_ARGS__)
#define NH_DEBUG(...) ::nh::logPrintf(::nh::LogLevel::Debug, __VA_ARGS__)

/// Monotonic clock in microseconds; used for every scheduling decision.
uint64_t nowMonotonicUs();
/// Wall clock in microseconds; only ever written into the published state.
uint64_t nowRealtimeUs();

/// $XDG_RUNTIME_DIR, or the same fallback Qt's StandardPaths uses.
std::string xdgRuntimeDir();

bool makeDirectories(const std::string &path, mode_t mode = 0700);
bool readWholeFile(const std::string &path, std::string &out, size_t limit = 1u << 20);
/// Writes via a temporary file + rename so readers never observe a partial document.
bool writeFileAtomically(const std::string &path, const std::string &data, mode_t mode = 0600);

bool setCloseOnExec(int fd);
bool setNonBlocking(int fd);

std::string base64Encode(const std::string &data);

std::string trimmed(const std::string &value);
bool hasSuffix(const std::string &value, const std::string &suffix);

} // namespace nh
