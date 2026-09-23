// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace nh {

/// A resolved ping target.
struct Endpoint {
    int family = AF_UNSPEC;
    sockaddr_storage addr {};
    socklen_t len = 0;
    std::string text; // numeric presentation form

    bool valid() const { return family != AF_UNSPEC && len > 0; }
    void clear() { *this = Endpoint(); }
    bool matches(const sockaddr_storage &other, socklen_t otherLen) const;
};

/// Parses a literal IPv4/IPv6 address (including "fe80::1%eth0"). Never blocks.
bool parseNumericAddress(const std::string &text, Endpoint &out);
/// Resolves a host name. Blocks - only ever called from the resolver thread.
bool resolveHostName(const std::string &host, Endpoint &out, std::string &error);
std::string formatAddress(const sockaddr_storage &addr, socklen_t len);

struct Reply {
    uint32_t uid = 0;
    uint32_t seq = 0;
    uint64_t sentUs = 0;
    sockaddr_storage from {};
    socklen_t fromLen = 0;
};

enum class SendResult {
    Sent,        ///< handed to the kernel
    Unreachable, ///< the kernel answered immediately (no route, host unreachable, ...)
    Failed,      ///< transient local failure, retry on the next tick
};

/// One ICMP socket per address family, shared by every destination.
///
/// Uses unprivileged ICMP datagram sockets (net.ipv4.ping_group_range) and
/// falls back to raw sockets when the process holds CAP_NET_RAW.
class IcmpSocket {
public:
    ~IcmpSocket();

    bool open(int family, std::string &error);
    void close();

    int fd() const { return m_fd; }
    bool isOpen() const { return m_fd >= 0; }
    bool isRaw() const { return m_raw; }
    int family() const { return m_family; }

    SendResult send(const Endpoint &endpoint, uint32_t uid, uint32_t seq, uint64_t sentUs, uint32_t cookie, std::string *error);

    /// Drains every datagram currently queued, appending valid echo replies.
    size_t receive(uint32_t cookie, std::vector<Reply> &out);

private:
    int m_fd = -1;
    int m_family = AF_UNSPEC;
    bool m_raw = false;
};

} // namespace nh
