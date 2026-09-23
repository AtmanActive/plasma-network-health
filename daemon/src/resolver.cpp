// SPDX-License-Identifier: MIT
#include "resolver.h"

#include "util.h"

#include <cerrno>
#include <cstring>
#include <iterator>
#include <sys/eventfd.h>
#include <unistd.h>

namespace nh {

Resolver::Resolver()
{
    m_eventFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventFd < 0) {
        NH_ERROR("eventfd() failed: %s", std::strerror(errno));
    }
    m_thread = std::thread([this] { run(); });
}

Resolver::~Resolver()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_condition.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_eventFd >= 0) {
        ::close(m_eventFd);
        m_eventFd = -1;
    }
}

void Resolver::request(uint32_t uid, const std::string &host)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Collapse duplicate in-flight requests for the same target.
        for (const Request &pending : m_pending) {
            if (pending.uid == uid) {
                return;
            }
        }
        m_pending.push_back(Request { uid, host });
    }
    m_condition.notify_one();
}

void Resolver::collect(std::vector<Result> &out)
{
    if (m_eventFd >= 0) {
        uint64_t counter = 0;
        while (::read(m_eventFd, &counter, sizeof(counter)) < 0 && errno == EINTR) {
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_completed.empty()) {
        return;
    }
    out.insert(out.end(), std::make_move_iterator(m_completed.begin()), std::make_move_iterator(m_completed.end()));
    m_completed.clear();
}

void Resolver::run()
{
    while (true) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stop || !m_pending.empty(); });
            if (m_stop) {
                return;
            }
            request = m_pending.front();
            m_pending.pop_front();
        }

        Result result;
        result.uid = request.uid;
        result.host = request.host;
        result.ok = resolveHostName(request.host, result.endpoint, result.error);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_completed.push_back(std::move(result));
        }
        if (m_eventFd >= 0) {
            const uint64_t one = 1;
            while (::write(m_eventFd, &one, sizeof(one)) < 0 && errno == EINTR) {
            }
        }
    }
}

} // namespace nh
