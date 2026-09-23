// SPDX-License-Identifier: MIT
//
// plasma-network-healthd - the ping engine behind the Network Health plasmoid.
//
// Design notes:
//   * One thread drives everything through a single epoll loop. The only
//     blocking call in the program (getaddrinfo) lives on a worker thread.
//   * All destinations share one ICMP socket per address family, and probes due
//     at the same moment are sent from one wakeup, so a hundred targets at 1 Hz
//     still cost roughly two wakeups per second.
//   * Nothing here ever touches the Plasma process: results are published to a
//     small JSON file that the widget reads.
#include "monitor.h"
#include "util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Set by the build; the fallback keeps the sources compilable on their own.
#ifndef NH_VERSION
#define NH_VERSION "1.0.0"
#endif

namespace {

const char *kVersion = NH_VERSION;

void printUsage(const char *argv0)
{
    std::printf(
        "plasma-network-healthd %s - ICMP monitoring backend for the Network Health plasmoid\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -d, --daemon              detach and run in the background\n"
        "      --runtime-dir DIR     base directory for config and state\n"
        "                            (default: $XDG_RUNTIME_DIR/plasma-network-health)\n"
        "      --idle-exit SECONDS   quit after being idle this long (0 = never, default 0)\n"
        "      --client-timeout SEC  forget widgets that stop refreshing their config\n"
        "                            (0 = never, default 900)\n"
        "      --publish-interval MS minimum delay between state file writes (default 250)\n"
        "      --log-file PATH       append diagnostics to PATH instead of stderr\n"
        "      --log-level LEVEL     error | warn | info | debug (default info)\n"
        "      --max-destinations N  per-widget destination cap (default 128)\n"
        "  -v, --version             print the version and exit\n"
        "  -h, --help                print this help and exit\n"
        "\n"
        "The daemon is single-instance per user: starting it again is a no-op.\n",
        kVersion, argv0);
}

bool parseLogLevel(const std::string &text, nh::LogLevel &out)
{
    if (text == "error") {
        out = nh::LogLevel::Error;
    } else if (text == "warn" || text == "warning") {
        out = nh::LogLevel::Warn;
    } else if (text == "info") {
        out = nh::LogLevel::Info;
    } else if (text == "debug") {
        out = nh::LogLevel::Debug;
    } else {
        return false;
    }
    return true;
}

/// flock() on a lock file in the runtime directory. The descriptor is inherited
/// across the daemonising forks, so the lock outlives the parent.
/// Returns the descriptor, which is deliberately kept open for the lifetime of
/// the process, or -1 if another instance holds the lock.
int acquireSingleInstanceLock(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        NH_ERROR("cannot open lock file %s: %s", path.c_str(), std::strerror(errno));
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// The lock file doubles as the pid file. It is written after daemonising,
/// because the process name is truncated to 15 characters in /proc, which makes
/// pkill-style matching unreliable - the pid here is the supported way to find
/// this daemon.
void writePidFile(int fd)
{
    if (fd < 0 || ::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) != 0) {
        return;
    }
    const std::string pid = std::to_string(::getpid()) + "\n";
    if (::write(fd, pid.data(), pid.size()) < 0) {
        // Purely informational; losing it does not affect correctness.
    }
}

/// Must run before any thread is created: a blocked mask is inherited by new
/// threads, and a signal that any thread leaves unblocked would be delivered
/// there and kill the process outright instead of reaching our signalfd.
bool blockShutdownSignals()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        NH_ERROR("sigprocmask: %s", std::strerror(errno));
        return false;
    }
    ::signal(SIGPIPE, SIG_IGN);
    return true;
}

bool daemonise()
{
    pid_t pid = ::fork();
    if (pid < 0) {
        NH_ERROR("fork: %s", std::strerror(errno));
        return false;
    }
    if (pid > 0) {
        ::_exit(0);
    }
    if (::setsid() < 0) {
        NH_ERROR("setsid: %s", std::strerror(errno));
        return false;
    }
    pid = ::fork();
    if (pid < 0) {
        NH_ERROR("fork: %s", std::strerror(errno));
        return false;
    }
    if (pid > 0) {
        ::_exit(0);
    }
    if (::chdir("/") != 0) {
        NH_WARN("chdir(/): %s", std::strerror(errno));
    }
    ::umask(022);
    return true;
}

void redirectStandardStreams(const std::string &logFile)
{
    const int nullFd = ::open("/dev/null", O_RDWR);
    if (nullFd >= 0) {
        ::dup2(nullFd, STDIN_FILENO);
        if (nullFd > STDERR_FILENO) {
            ::close(nullFd);
        }
    }

    int outFd = -1;
    if (!logFile.empty()) {
        outFd = ::open(logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (outFd < 0) {
            NH_WARN("cannot open log file %s: %s", logFile.c_str(), std::strerror(errno));
        }
    }
    if (outFd < 0) {
        outFd = ::open("/dev/null", O_WRONLY);
    }
    if (outFd >= 0) {
        ::dup2(outFd, STDOUT_FILENO);
        ::dup2(outFd, STDERR_FILENO);
        if (outFd > STDERR_FILENO) {
            ::close(outFd);
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    nh::MonitorOptions options;
    options.runtimeDir = nh::xdgRuntimeDir() + "/plasma-network-health";

    bool background = false;
    std::string logFile;

    auto requireValue = [&](int &i, const char *name) -> const char * {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s requires an argument\n", name);
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (argument == "-v" || argument == "--version") {
            std::printf("%s\n", kVersion);
            return 0;
        }
        if (argument == "-d" || argument == "--daemon") {
            background = true;
        } else if (argument == "--runtime-dir") {
            options.runtimeDir = requireValue(i, "--runtime-dir");
        } else if (argument == "--idle-exit") {
            options.idleExitUs = uint64_t(std::strtoull(requireValue(i, "--idle-exit"), nullptr, 10)) * 1000000ull;
        } else if (argument == "--client-timeout") {
            options.clientTimeoutUs = uint64_t(std::strtoull(requireValue(i, "--client-timeout"), nullptr, 10)) * 1000000ull;
        } else if (argument == "--publish-interval") {
            options.publishIntervalUs = uint64_t(std::strtoull(requireValue(i, "--publish-interval"), nullptr, 10)) * 1000ull;
        } else if (argument == "--max-destinations") {
            options.maxDestinations = size_t(std::strtoull(requireValue(i, "--max-destinations"), nullptr, 10));
        } else if (argument == "--log-file") {
            logFile = requireValue(i, "--log-file");
        } else if (argument == "--log-level") {
            nh::LogLevel level = nh::LogLevel::Info;
            const char *value = requireValue(i, "--log-level");
            if (!parseLogLevel(value, level)) {
                std::fprintf(stderr, "unknown log level '%s'\n", value);
                return 2;
            }
            nh::logSetLevel(level);
        } else {
            std::fprintf(stderr, "unknown option '%s' (try --help)\n", argument.c_str());
            return 2;
        }
    }

    if (options.maxDestinations == 0 || options.maxDestinations > 4096) {
        options.maxDestinations = 128;
    }
    if (options.publishIntervalUs > 10000000ull) {
        options.publishIntervalUs = 10000000ull;
    }

    if (!nh::makeDirectories(options.runtimeDir)) {
        std::fprintf(stderr, "cannot create %s\n", options.runtimeDir.c_str());
        return 1;
    }

    const int lockFd = acquireSingleInstanceLock(options.runtimeDir + "/daemon.lock");
    if (lockFd < 0) {
        NH_INFO("another instance is already running, nothing to do");
        return 0;
    }

    if (background && !daemonise()) {
        return 1;
    }
    writePidFile(lockFd);
    if (background || !logFile.empty()) {
        redirectStandardStreams(logFile);
    }

    NH_INFO("plasma-network-healthd %s starting (runtime dir: %s)", kVersion, options.runtimeDir.c_str());

    if (!blockShutdownSignals()) {
        return 1;
    }

    nh::Monitor monitor(options);
    std::string error;
    if (!monitor.start(error)) {
        NH_ERROR("startup failed: %s", error.c_str());
        return 1;
    }

    const int result = monitor.run();
    NH_INFO("plasma-network-healthd stopped");
    return result;
}
