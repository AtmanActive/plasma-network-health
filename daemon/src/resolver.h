// SPDX-License-Identifier: MIT
#pragma once

#include "pinger.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nh {

/// Host name resolution on a worker thread.
///
/// getaddrinfo() is the only blocking call in the daemon, so it is kept off the
/// event loop entirely. Completions wake the loop through an eventfd.
class Resolver {
public:
    struct Result {
        uint32_t uid = 0;
        std::string host;
        bool ok = false;
        Endpoint endpoint;
        std::string error;
    };

    Resolver();
    ~Resolver();

    /// eventfd that becomes readable once results are waiting.
    int notifyFd() const { return m_eventFd; }

    void request(uint32_t uid, const std::string &host);
    /// Drains completed lookups; also clears the eventfd.
    void collect(std::vector<Result> &out);

private:
    void run();

    struct Request {
        uint32_t uid;
        std::string host;
    };

    int m_eventFd = -1;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<Request> m_pending;
    std::vector<Result> m_completed;
    bool m_stop = false;
};

} // namespace nh
