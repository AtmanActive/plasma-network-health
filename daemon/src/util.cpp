// SPDX-License-Identifier: MIT
#include "util.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace nh {
namespace {

LogLevel g_level = LogLevel::Info;

const char *levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Debug: return "DEBUG";
    }
    return "?????";
}

} // namespace

void logSetLevel(LogLevel level)
{
    g_level = level;
}

LogLevel logLevel()
{
    return g_level;
}

void logPrintf(LogLevel level, const char *fmt, ...)
{
    if (static_cast<int>(level) > static_cast<int>(g_level)) {
        return;
    }
    char stamp[32] = "";
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv {};
    if (localtime_r(&ts.tv_sec, &tmv)) {
        std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000);
    }

    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::fprintf(stderr, "[%s] %s %s\n", stamp, levelName(level), message);
    std::fflush(stderr);
}

uint64_t nowMonotonicUs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

uint64_t nowRealtimeUs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

std::string xdgRuntimeDir()
{
    const char *env = ::getenv("XDG_RUNTIME_DIR");
    if (env && *env) {
        return std::string(env);
    }
    // Mirror QStandardPaths::RuntimeLocation's fallback so the plasmoid and the
    // daemon always agree on the path without having to pass it around.
    std::string name;
    if (const struct passwd *pw = ::getpwuid(::getuid())) {
        if (pw->pw_name) {
            name = pw->pw_name;
        }
    }
    if (name.empty()) {
        name = std::to_string(static_cast<unsigned>(::getuid()));
    }
    return "/tmp/runtime-" + name;
}

bool makeDirectories(const std::string &path, mode_t mode)
{
    if (path.empty()) {
        return false;
    }
    std::string current;
    current.reserve(path.size());
    size_t index = 0;
    if (path[0] == '/') {
        current = "/";
        index = 1;
    }
    while (index <= path.size()) {
        const size_t slash = path.find('/', index);
        const std::string component = path.substr(index, slash == std::string::npos ? std::string::npos : slash - index);
        if (!component.empty()) {
            if (current.size() > 1 || (current.size() == 1 && current[0] != '/')) {
                current += '/';
            }
            current += component;
            if (::mkdir(current.c_str(), mode) != 0 && errno != EEXIST) {
                NH_ERROR("mkdir(%s) failed: %s", current.c_str(), std::strerror(errno));
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        index = slash + 1;
    }
    return true;
}

bool readWholeFile(const std::string &path, std::string &out, size_t limit)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    out.clear();
    char buffer[8192];
    while (out.size() < limit) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        out.append(buffer, size_t(n));
    }
    ::close(fd);
    return true;
}

bool writeFileAtomically(const std::string &path, const std::string &data, mode_t mode)
{
    std::string temp = path + ".tmpXXXXXX";
    std::vector<char> buffer(temp.begin(), temp.end());
    buffer.push_back('\0');

    const int fd = ::mkstemp(buffer.data());
    if (fd < 0) {
        NH_ERROR("mkstemp(%s) failed: %s", buffer.data(), std::strerror(errno));
        return false;
    }
    temp.assign(buffer.data());

    bool ok = true;
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            NH_ERROR("write(%s) failed: %s", temp.c_str(), std::strerror(errno));
            ok = false;
            break;
        }
        written += size_t(n);
    }
    if (ok) {
        ok = ::fchmod(fd, mode) == 0;
    }
    ::close(fd);

    if (ok && ::rename(temp.c_str(), path.c_str()) != 0) {
        NH_ERROR("rename(%s -> %s) failed: %s", temp.c_str(), path.c_str(), std::strerror(errno));
        ok = false;
    }
    if (!ok) {
        ::unlink(temp.c_str());
    }
    return ok;
}

bool setCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string base64Encode(const std::string &data)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t triple = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) | uint32_t(uint8_t(data[i + 2]));
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(alphabet[(triple >> 6) & 0x3F]);
        out.push_back(alphabet[triple & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t triple = uint32_t(uint8_t(data[i])) << 16;
        const bool haveSecond = (i + 1) < data.size();
        if (haveSecond) {
            triple |= uint32_t(uint8_t(data[i + 1])) << 8;
        }
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(haveSecond ? alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string trimmed(const std::string &value)
{
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool hasSuffix(const std::string &value, const std::string &suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace nh
