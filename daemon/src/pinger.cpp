// SPDX-License-Identifier: MIT
#include "pinger.h"

#include "util.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <unistd.h>

namespace nh {
namespace {

constexpr uint32_t kMagic = 0x504E4831u; // "PNH1"
constexpr int kReceiveBatch = 32;
constexpr size_t kMaxDatagram = 256;

/// Echoed back verbatim by the peer, so it carries everything needed to match a
/// reply to its request without keeping any per-packet bookkeeping.
struct Payload {
    uint32_t magic;
    uint32_t cookie;
    uint32_t uid;
    uint32_t seq;
    uint64_t sentUs;
};
static_assert(sizeof(Payload) == 24, "unexpected payload padding");

struct IcmpHeader {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
};
static_assert(sizeof(IcmpHeader) == 8, "unexpected ICMP header padding");

constexpr size_t kPacketSize = sizeof(IcmpHeader) + sizeof(Payload);

uint16_t internetChecksum(const void *data, size_t length)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint32_t sum = 0;
    while (length > 1) {
        uint16_t word;
        std::memcpy(&word, bytes, 2);
        sum += word;
        bytes += 2;
        length -= 2;
    }
    if (length == 1) {
        uint16_t last = 0;
        std::memcpy(&last, bytes, 1);
        sum += last;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

bool isUnreachableErrno(int err)
{
    switch (err) {
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
    case EHOSTDOWN:
    case ECONNREFUSED:
    case EACCES:
    case EPERM:
    case EINVAL:
    case EAFNOSUPPORT:
        return true;
    default:
        return false;
    }
}

} // namespace

bool Endpoint::matches(const sockaddr_storage &other, socklen_t otherLen) const
{
    if (!valid() || otherLen == 0) {
        return false;
    }
    if (other.ss_family != addr.ss_family) {
        return false;
    }
    if (addr.ss_family == AF_INET) {
        const sockaddr_in *a = reinterpret_cast<const sockaddr_in *>(&addr);
        const sockaddr_in *b = reinterpret_cast<const sockaddr_in *>(&other);
        return a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (addr.ss_family == AF_INET6) {
        const sockaddr_in6 *a = reinterpret_cast<const sockaddr_in6 *>(&addr);
        const sockaddr_in6 *b = reinterpret_cast<const sockaddr_in6 *>(&other);
        return std::memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(in6_addr)) == 0;
    }
    return false;
}

static bool lookup(const std::string &host, int flags, Endpoint &out, std::string &error)
{
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = flags;

    struct addrinfo *results = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (rc != 0 || !results) {
        error = ::gai_strerror(rc);
        if (results) {
            ::freeaddrinfo(results);
        }
        return false;
    }

    // Prefer IPv4 when a host offers both: it is the cheaper path on most
    // desktops and matches what users expect from a plain "ping".
    const struct addrinfo *chosen = nullptr;
    for (const struct addrinfo *it = results; it; it = it->ai_next) {
        if (it->ai_family == AF_INET) {
            chosen = it;
            break;
        }
        if (!chosen && it->ai_family == AF_INET6) {
            chosen = it;
        }
    }
    if (!chosen) {
        ::freeaddrinfo(results);
        error = "no usable address";
        return false;
    }

    out.clear();
    out.family = chosen->ai_family;
    out.len = chosen->ai_addrlen;
    std::memcpy(&out.addr, chosen->ai_addr, chosen->ai_addrlen);
    out.text = formatAddress(out.addr, out.len);
    ::freeaddrinfo(results);
    error.clear();
    return true;
}

bool parseNumericAddress(const std::string &text, Endpoint &out)
{
    if (text.empty()) {
        return false;
    }
    std::string error;
    return lookup(text, AI_NUMERICHOST | AI_NUMERICSERV, out, error);
}

bool resolveHostName(const std::string &host, Endpoint &out, std::string &error)
{
    return lookup(host, AI_ADDRCONFIG, out, error);
}

std::string formatAddress(const sockaddr_storage &addr, socklen_t len)
{
    char host[NI_MAXHOST] = "";
    if (::getnameinfo(reinterpret_cast<const sockaddr *>(&addr), len, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) {
        return std::string();
    }
    return std::string(host);
}

IcmpSocket::~IcmpSocket()
{
    close();
}

bool IcmpSocket::open(int family, std::string &error)
{
    close();
    m_family = family;

    const int protocol = (family == AF_INET6) ? int(IPPROTO_ICMPV6) : int(IPPROTO_ICMP);

    m_fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    m_raw = false;
    if (m_fd < 0) {
        const int dgramErrno = errno;
        // Unprivileged ICMP is gated by net.ipv4.ping_group_range; fall back to
        // a raw socket, which works when the binary carries cap_net_raw.
        m_fd = ::socket(family, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (m_fd < 0) {
            error = std::string("ICMP socket unavailable (datagram: ") + std::strerror(dgramErrno) + ", raw: " + std::strerror(errno) + ")";
            m_family = AF_UNSPEC;
            return false;
        }
        m_raw = true;
    }

    // Keep the kernel receive queue small: stale replies are worthless to us and
    // a large backlog would only delay the fresh ones.
    int bufferSize = 64 * 1024;
    ::setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));

    if (family == AF_INET6) {
        // Only echo replies matter; let the kernel discard everything else.
        struct icmp6_filter filter;
        ICMP6_FILTER_SETBLOCKALL(&filter);
        ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
        ::setsockopt(m_fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter));
    }

    error.clear();
    return true;
}

void IcmpSocket::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_family = AF_UNSPEC;
    m_raw = false;
}

SendResult IcmpSocket::send(const Endpoint &endpoint, uint32_t uid, uint32_t seq, uint64_t sentUs, uint32_t cookie, std::string *error)
{
    if (m_fd < 0 || !endpoint.valid() || endpoint.family != m_family) {
        if (error) {
            *error = "socket not ready for this address family";
        }
        return SendResult::Failed;
    }

    uint8_t packet[kPacketSize];
    IcmpHeader header {};
    header.type = (m_family == AF_INET6) ? ICMP6_ECHO_REQUEST : ICMP_ECHO;
    header.code = 0;
    header.checksum = 0;
    // The kernel rewrites the identifier on datagram sockets; the payload is
    // what we actually match replies against, so these two are cosmetic.
    header.id = htons(static_cast<uint16_t>(uid & 0xFFFF));
    header.sequence = htons(static_cast<uint16_t>(seq & 0xFFFF));

    Payload payload {};
    payload.magic = kMagic;
    payload.cookie = cookie;
    payload.uid = uid;
    payload.seq = seq;
    payload.sentUs = sentUs;

    std::memcpy(packet, &header, sizeof(header));
    std::memcpy(packet + sizeof(header), &payload, sizeof(payload));

    // IPv6 checksums are always computed by the kernel, and so are IPv4 ones on
    // datagram sockets (it has to, since it rewrites the identifier).
    if (m_raw && m_family == AF_INET) {
        const uint16_t sum = internetChecksum(packet, sizeof(packet));
        std::memcpy(packet + offsetof(IcmpHeader, checksum), &sum, sizeof(sum));
    }

    while (true) {
        const ssize_t n = ::sendto(m_fd, packet, sizeof(packet), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&endpoint.addr), endpoint.len);
        if (n == ssize_t(sizeof(packet))) {
            return SendResult::Sent;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        const int err = errno;
        if (error) {
            *error = std::strerror(err);
        }
        // A queued ICMP error (no route, unreachable) surfaces here on the next
        // send, which lets us report a dead target without waiting for a timeout.
        return isUnreachableErrno(err) ? SendResult::Unreachable : SendResult::Failed;
    }
}

size_t IcmpSocket::receive(uint32_t cookie, std::vector<Reply> &out)
{
    if (m_fd < 0) {
        return 0;
    }

    size_t accepted = 0;
    const uint8_t expectedType = (m_family == AF_INET6) ? ICMP6_ECHO_REPLY : ICMP_ECHOREPLY;

    struct mmsghdr messages[kReceiveBatch];
    struct iovec iovecs[kReceiveBatch];
    static thread_local uint8_t buffers[kReceiveBatch][kMaxDatagram];
    sockaddr_storage sources[kReceiveBatch];

    while (true) {
        std::memset(messages, 0, sizeof(messages));
        for (int i = 0; i < kReceiveBatch; ++i) {
            iovecs[i].iov_base = buffers[i];
            iovecs[i].iov_len = kMaxDatagram;
            messages[i].msg_hdr.msg_iov = &iovecs[i];
            messages[i].msg_hdr.msg_iovlen = 1;
            messages[i].msg_hdr.msg_name = &sources[i];
            messages[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
        }

        const int count = ::recvmmsg(m_fd, messages, kReceiveBatch, MSG_DONTWAIT, nullptr);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            // EAGAIN simply means the queue is drained.
            break;
        }
        if (count == 0) {
            break;
        }

        for (int i = 0; i < count; ++i) {
            const uint8_t *data = buffers[i];
            size_t length = messages[i].msg_len;

            // Raw IPv4 sockets hand us the IP header too; IPv6 never does.
            if (m_raw && m_family == AF_INET) {
                if (length < sizeof(struct iphdr)) {
                    continue;
                }
                const size_t headerLength = size_t(data[0] & 0x0F) * 4;
                if (headerLength < sizeof(struct iphdr) || length < headerLength) {
                    continue;
                }
                data += headerLength;
                length -= headerLength;
            }

            if (length < kPacketSize) {
                continue;
            }

            IcmpHeader header {};
            std::memcpy(&header, data, sizeof(header));
            if (header.type != expectedType || header.code != 0) {
                continue;
            }

            Payload payload {};
            std::memcpy(&payload, data + sizeof(header), sizeof(payload));
            if (payload.magic != kMagic || payload.cookie != cookie) {
                continue;
            }

            Reply reply;
            reply.uid = payload.uid;
            reply.seq = payload.seq;
            reply.sentUs = payload.sentUs;
            reply.fromLen = messages[i].msg_hdr.msg_namelen;
            std::memcpy(&reply.from, &sources[i], sizeof(sockaddr_storage));
            out.push_back(reply);
            ++accepted;
        }

        if (count < kReceiveBatch) {
            break;
        }
    }

    return accepted;
}

} // namespace nh
