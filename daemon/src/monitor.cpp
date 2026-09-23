// SPDX-License-Identifier: MIT
#include "monitor.h"

#include "json.hpp"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <random>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nh {
namespace {

/// Probes due within this window are sent from the same wakeup, so a hundred
/// destinations sharing an interval cost one timer expiry rather than a hundred.
constexpr uint64_t kSendSlackUs = 2000;
constexpr uint64_t kResolveTtlUs = 300ull * 1000000ull;
constexpr uint64_t kResolveBackoffBaseUs = 5ull * 1000000ull;
constexpr uint64_t kResolveBackoffMaxUs = 300ull * 1000000ull;
constexpr uint64_t kClientCheckIntervalUs = 60ull * 1000000ull;
/// state.json exists for people, not for the widget, so it is refreshed lazily.
constexpr uint64_t kReadablePublishIntervalUs = 5ull * 1000000ull;
constexpr uint64_t kNeverUs = ~uint64_t(0);

template<typename T>
T clampValue(T value, T low, T high)
{
    return value < low ? low : (value > high ? high : value);
}

std::string proberKey(const std::string &host, uint64_t intervalUs, uint64_t timeoutUs)
{
    return host + '\x1f' + std::to_string(intervalUs) + '\x1f' + std::to_string(timeoutUs);
}

} // namespace

const char *healthStateName(HealthState state)
{
    switch (state) {
    case HealthState::Good: return "good";
    case HealthState::Bad: return "bad";
    case HealthState::Unknown: break;
    }
    return "unknown";
}

Monitor::Monitor(MonitorOptions options)
    : m_options(std::move(options))
{
}

Monitor::~Monitor()
{
    if (m_signalFd >= 0) {
        ::close(m_signalFd);
    }
    if (m_inotifyFd >= 0) {
        ::close(m_inotifyFd);
    }
    if (m_epollFd >= 0) {
        ::close(m_epollFd);
    }
    ::unlink(stateFilePath().c_str());
    ::unlink(stateIniPath().c_str());
}

bool Monitor::openSockets()
{
    const bool ok4 = m_socket4.open(AF_INET, m_error4);
    const bool ok6 = m_socket6.open(AF_INET6, m_error6);

    if (ok4) {
        NH_INFO("IPv4 ICMP socket ready (%s)", m_socket4.isRaw() ? "raw" : "unprivileged datagram");
    } else {
        NH_WARN("IPv4 ICMP unavailable: %s", m_error4.c_str());
    }
    if (ok6) {
        NH_INFO("IPv6 ICMP socket ready (%s)", m_socket6.isRaw() ? "raw" : "unprivileged datagram");
    } else {
        NH_WARN("IPv6 ICMP unavailable: %s", m_error6.c_str());
    }
    return ok4 || ok6;
}

bool Monitor::start(std::string &error)
{
    m_startedRealUs = nowRealtimeUs();

    if (!makeDirectories(m_options.runtimeDir) || !makeDirectories(clientsDirPath())) {
        error = "cannot create runtime directory " + m_options.runtimeDir;
        return false;
    }

    std::random_device device;
    m_cookie = uint32_t(device()) | 1u;

    if (!openSockets()) {
        // Not fatal: the state file reports the failure so the widget can show
        // something useful instead of silently staying grey.
        NH_ERROR("no ICMP socket could be opened; check net.ipv4.ping_group_range");
    }

    m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_epollFd < 0) {
        error = std::string("epoll_create1: ") + std::strerror(errno);
        return false;
    }

    // The caller has already blocked these process-wide - it has to, before any
    // thread exists. Blocking them again here keeps this self-contained.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        error = "pthread_sigmask failed";
        return false;
    }
    m_signalFd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (m_signalFd < 0) {
        error = std::string("signalfd: ") + std::strerror(errno);
        return false;
    }

    m_inotifyFd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (m_inotifyFd < 0) {
        error = std::string("inotify_init1: ") + std::strerror(errno);
        return false;
    }
    m_inotifyWatch = ::inotify_add_watch(m_inotifyFd, clientsDirPath().c_str(),
                                         IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
    if (m_inotifyWatch < 0) {
        NH_WARN("inotify_add_watch(%s): %s", clientsDirPath().c_str(), std::strerror(errno));
    }

    auto addFd = [this](int fd) {
        if (fd < 0) {
            return;
        }
        struct epoll_event event {};
        event.events = EPOLLIN;
        event.data.fd = fd;
        if (::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &event) != 0) {
            NH_ERROR("epoll_ctl(ADD, %d): %s", fd, std::strerror(errno));
        }
    };
    addFd(m_signalFd);
    addFd(m_inotifyFd);
    addFd(m_resolver.notifyFd());
    addFd(m_socket4.fd());
    addFd(m_socket6.fd());

    reloadClients();
    publish(nowMonotonicUs(), true);

    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

bool Monitor::loadClientFile(const std::string &path, const std::string &id, Client &out) const
{
    std::string text;
    if (!readWholeFile(path, text)) {
        return false;
    }

    std::string parseError;
    const Json document = Json::parse(text, &parseError);
    if (!document.isObject()) {
        NH_WARN("client %s: %s", id.c_str(), parseError.empty() ? "not a JSON object" : parseError.c_str());
        return false;
    }

    out = Client();
    out.id = id;
    out.path = path;
    out.intervalUs = uint64_t(clampValue<long long>(document["interval_ms"].toInt(1000), 100, 3600000)) * 1000ull;
    out.timeoutUs = uint64_t(clampValue<long long>(document["timeout_ms"].toInt(static_cast<long long>(out.intervalUs / 1000)), 50, 60000)) * 1000ull;

    const Json &list = document["destinations"];
    if (!list.isArray()) {
        return true;
    }

    for (size_t i = 0; i < list.size() && out.destinations.size() < m_options.maxDestinations; ++i) {
        const Json &entry = list.at(i);
        if (!entry.isObject()) {
            continue;
        }
        Destination destination;
        destination.address = trimmed(entry["address"].toString());
        if (destination.address.empty()) {
            continue;
        }
        destination.id = entry["id"].toString();
        if (destination.id.empty()) {
            destination.id = "d" + std::to_string(i);
        }
        destination.name = entry["name"].toString(destination.address);
        destination.order = int(entry["order"].toInt(static_cast<long long>(i)));
        destination.thresholdUs = uint64_t(clampValue<long long>(entry["threshold_us"].toInt(1000), 1, 60000000));
        destination.sensitivity = int(clampValue<long long>(entry["sensitivity"].toInt(3), 0, 1000));
        destination.enabled = entry["enabled"].isNull() ? true : entry["enabled"].toBool(true);
        out.destinations.push_back(std::move(destination));
    }

    return true;
}

void Monitor::reloadClients()
{
    m_configDirty = false;

    std::vector<Client> loaded;
    DIR *dir = ::opendir(clientsDirPath().c_str());
    if (dir) {
        while (const struct dirent *entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.empty() || name[0] == '.' || !hasSuffix(name, ".json")) {
                continue;
            }
            const std::string id = name.substr(0, name.size() - 5);
            Client client;
            if (loadClientFile(clientsDirPath() + "/" + name, id, client)) {
                loaded.push_back(std::move(client));
            }
        }
        ::closedir(dir);
    } else if (errno != ENOENT) {
        NH_WARN("opendir(%s): %s", clientsDirPath().c_str(), std::strerror(errno));
    } else {
        // The directory vanished (logout, manual cleanup): recreate and rewatch.
        makeDirectories(clientsDirPath());
        if (m_inotifyFd >= 0) {
            m_inotifyWatch = ::inotify_add_watch(m_inotifyFd, clientsDirPath().c_str(),
                                                 IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
        }
    }

    std::sort(loaded.begin(), loaded.end(), [](const Client &a, const Client &b) { return a.id < b.id; });

    // Carry over the evaluated state of destinations that survived the edit, so
    // a rename or a threshold tweak does not blink every LED back to grey.
    for (Client &client : loaded) {
        const auto previous = std::find_if(m_clients.begin(), m_clients.end(), [&](const Client &c) { return c.id == client.id; });
        if (previous == m_clients.end()) {
            continue;
        }
        for (Destination &destination : client.destinations) {
            const auto old = std::find_if(previous->destinations.begin(), previous->destinations.end(), [&](const Destination &d) {
                return d.id == destination.id && d.address == destination.address;
            });
            if (old == previous->destinations.end()) {
                continue;
            }
            destination.state = old->state;
            destination.goodStreak = old->goodStreak;
            destination.badStreak = old->badStreak;
            destination.haveRtt = old->haveRtt;
            destination.rttUs = old->rttUs;
            destination.sent = old->sent;
            destination.received = old->received;
            destination.lastReplyRealUs = old->lastReplyRealUs;
            destination.lastChangeRealUs = old->lastChangeRealUs;
        }
    }

    size_t destinationCount = 0;
    for (const Client &client : loaded) {
        destinationCount += client.destinations.size();
    }
    NH_INFO("configuration reloaded: %zu client(s), %zu destination(s)", loaded.size(), destinationCount);

    m_clients = std::move(loaded);
    rebuildProbers();
    m_stateDirty = true;
}

void Monitor::rebuildProbers()
{
    std::vector<Prober> previous = std::move(m_probers);
    std::vector<bool> consumed(previous.size(), false);
    std::unordered_map<std::string, size_t> previousByKey;
    for (size_t i = 0; i < previous.size(); ++i) {
        previousByKey.emplace(proberKey(previous[i].host, previous[i].intervalUs, previous[i].timeoutUs), i);
    }

    m_probers.clear();
    m_proberByUid.clear();
    std::unordered_map<std::string, size_t> currentByKey;
    const uint64_t now = nowMonotonicUs();

    for (size_t c = 0; c < m_clients.size(); ++c) {
        Client &client = m_clients[c];
        for (size_t d = 0; d < client.destinations.size(); ++d) {
            Destination &destination = client.destinations[d];
            destination.prober = -1;
            if (!destination.enabled) {
                continue;
            }

            const std::string key = proberKey(destination.address, client.intervalUs, client.timeoutUs);
            auto existing = currentByKey.find(key);
            if (existing == currentByKey.end()) {
                Prober prober;
                const auto recycled = previousByKey.find(key);
                if (recycled != previousByKey.end() && !consumed[recycled->second]) {
                    prober = std::move(previous[recycled->second]);
                    consumed[recycled->second] = true;
                    prober.subscribers.clear();
                } else {
                    prober.host = destination.address;
                    prober.intervalUs = client.intervalUs;
                    prober.timeoutUs = client.timeoutUs;
                    prober.uid = m_nextUid++;
                    prober.numeric = parseNumericAddress(destination.address, prober.endpoint);
                    prober.nextSendUs = now;
                    if (!prober.numeric) {
                        prober.nextResolveUs = now;
                        prober.lastError = "resolving";
                    }
                }
                m_probers.push_back(std::move(prober));
                existing = currentByKey.emplace(key, m_probers.size() - 1).first;
            }

            m_probers[existing->second].subscribers.push_back(Prober::Subscriber { c, d });
            destination.prober = int(existing->second);
        }
    }

    m_minIntervalUs = 0;
    for (size_t i = 0; i < m_probers.size(); ++i) {
        m_proberByUid[m_probers[i].uid] = i;
        if (m_minIntervalUs == 0 || m_probers[i].intervalUs < m_minIntervalUs) {
            m_minIntervalUs = m_probers[i].intervalUs;
        }
    }

    NH_DEBUG("%zu prober(s) after rebuild", m_probers.size());
}

void Monitor::handleInotify()
{
    alignas(struct inotify_event) char buffer[4096];
    while (true) {
        const ssize_t n = ::read(m_inotifyFd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        for (ssize_t offset = 0; offset < n;) {
            const struct inotify_event *event = reinterpret_cast<const struct inotify_event *>(buffer + offset);
            if (event->len == 0 || hasSuffix(event->name, ".json") || (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF))) {
                m_configDirty = true;
            }
            offset += ssize_t(sizeof(struct inotify_event) + event->len);
        }
    }
}

uint64_t Monitor::clientCheckIntervalUs() const
{
    if (m_options.clientTimeoutUs == 0) {
        return kClientCheckIntervalUs;
    }
    return std::min(kClientCheckIntervalUs, std::max<uint64_t>(m_options.clientTimeoutUs / 2, 1000000ull));
}

void Monitor::dropExpiredClients(uint64_t now)
{
    if (m_options.clientTimeoutUs == 0 || now - m_lastClientCheckUs < clientCheckIntervalUs()) {
        return;
    }
    m_lastClientCheckUs = now;

    const uint64_t realNow = nowRealtimeUs();
    bool removed = false;
    for (const Client &client : m_clients) {
        struct stat info {};
        if (::stat(client.path.c_str(), &info) != 0) {
            continue; // already gone, inotify will have flagged it
        }
        const uint64_t mtimeUs = uint64_t(info.st_mtim.tv_sec) * 1000000ull + uint64_t(info.st_mtim.tv_nsec) / 1000ull;
        if (realNow > mtimeUs && realNow - mtimeUs > m_options.clientTimeoutUs) {
            // The widget refreshes its config file periodically; a stale file
            // means its plasmashell went away without cleaning up.
            NH_INFO("dropping stale client %s (config not refreshed)", client.id.c_str());
            ::unlink(client.path.c_str());
            removed = true;
        }
    }
    if (removed) {
        m_configDirty = true;
    }
}

// ---------------------------------------------------------------------------
// Probing
// ---------------------------------------------------------------------------

void Monitor::applySample(Prober &prober, bool received, uint64_t rttUs, uint64_t now)
{
    (void)now;
    const uint64_t realNow = nowRealtimeUs();

    ++prober.sent;
    if (received) {
        ++prober.received;
    }

    for (const Prober::Subscriber &subscriber : prober.subscribers) {
        if (subscriber.client >= m_clients.size()) {
            continue;
        }
        Client &client = m_clients[subscriber.client];
        if (subscriber.destination >= client.destinations.size()) {
            continue;
        }
        Destination &destination = client.destinations[subscriber.destination];

        ++destination.sent;
        if (received) {
            ++destination.received;
            destination.haveRtt = true;
            destination.rttUs = rttUs;
            destination.lastReplyRealUs = realNow;
        } else {
            destination.haveRtt = false;
        }

        const bool good = received && rttUs <= destination.thresholdUs;
        if (good) {
            ++destination.goodStreak;
            destination.badStreak = 0;
        } else {
            ++destination.badStreak;
            destination.goodStreak = 0;
        }

        const HealthState before = destination.state;
        if (destination.state == HealthState::Unknown) {
            // First verdict is adopted immediately so a fresh widget shows a
            // colour within one interval instead of staying grey.
            destination.state = good ? HealthState::Good : HealthState::Bad;
        } else if (destination.state == HealthState::Good && destination.badStreak > destination.sensitivity) {
            destination.state = HealthState::Bad;
        } else if (destination.state == HealthState::Bad && destination.goodStreak > destination.sensitivity) {
            destination.state = HealthState::Good;
        }

        if (destination.state != before) {
            destination.lastChangeRealUs = realNow;
            m_stateDirty = true;
        }
    }

    m_dataDirty = true;
}

void Monitor::sendProbe(Prober &prober, uint64_t now)
{
    IcmpSocket &socket = (prober.endpoint.family == AF_INET6) ? m_socket6 : m_socket4;

    if (!prober.endpoint.valid() || !socket.isOpen()) {
        if (!prober.endpoint.valid() && prober.lastError.empty()) {
            prober.lastError = "unresolved address";
        } else if (!socket.isOpen()) {
            prober.lastError = (prober.endpoint.family == AF_INET6) ? m_error6 : m_error4;
        }
        applySample(prober, false, 0, now);
        return;
    }

    const uint32_t sequence = ++prober.sequence;
    std::string sendError;
    const SendResult result = socket.send(prober.endpoint, prober.uid, sequence, now, m_cookie, &sendError);

    switch (result) {
    case SendResult::Sent:
        prober.pending = true;
        prober.pendingSeq = sequence;
        prober.pendingDeadlineUs = now + prober.timeoutUs;
        if (prober.lastError == "unresolved address") {
            prober.lastError.clear();
        }
        break;
    case SendResult::Unreachable:
        prober.lastError = sendError;
        applySample(prober, false, 0, now);
        break;
    case SendResult::Failed:
        NH_DEBUG("send to %s failed: %s", prober.host.c_str(), sendError.c_str());
        prober.lastError = sendError;
        applySample(prober, false, 0, now);
        break;
    }
}

void Monitor::tick(uint64_t now)
{
    for (Prober &prober : m_probers) {
        if (prober.pending && now + kSendSlackUs >= prober.pendingDeadlineUs) {
            prober.pending = false;
            prober.lastError = "timeout";
            applySample(prober, false, 0, now);
        }

        if (!prober.numeric && !prober.resolving && now + kSendSlackUs >= prober.nextResolveUs) {
            prober.resolving = true;
            m_resolver.request(prober.uid, prober.host);
        }

        if (!prober.pending && now + kSendSlackUs >= prober.nextSendUs) {
            sendProbe(prober, now);
            prober.nextSendUs += prober.intervalUs;
            if (prober.nextSendUs <= now) {
                // The machine was suspended or badly overloaded; resynchronise
                // instead of firing a burst of catch-up probes.
                prober.nextSendUs = now + prober.intervalUs;
            }
        }
    }
}

void Monitor::handleReplies(uint64_t now)
{
    m_replyScratch.clear();
    m_socket4.receive(m_cookie, m_replyScratch);
    m_socket6.receive(m_cookie, m_replyScratch);

    for (const Reply &reply : m_replyScratch) {
        const auto it = m_proberByUid.find(reply.uid);
        if (it == m_proberByUid.end()) {
            continue;
        }
        Prober &prober = m_probers[it->second];
        if (!prober.pending || reply.seq != prober.pendingSeq) {
            continue; // duplicate, or a reply that arrived after its timeout
        }
        if (!prober.endpoint.matches(reply.from, reply.fromLen)) {
            continue;
        }
        prober.pending = false;
        prober.lastError.clear();
        const uint64_t rtt = (now > reply.sentUs) ? (now - reply.sentUs) : 0;
        applySample(prober, true, rtt, now);
    }
}

void Monitor::handleResolutions(uint64_t now)
{
    std::vector<Resolver::Result> results;
    m_resolver.collect(results);

    for (Resolver::Result &result : results) {
        const auto it = m_proberByUid.find(result.uid);
        if (it == m_proberByUid.end()) {
            continue;
        }
        Prober &prober = m_probers[it->second];
        prober.resolving = false;

        if (result.ok) {
            if (!prober.endpoint.valid() || prober.endpoint.text != result.endpoint.text) {
                NH_INFO("%s resolved to %s", prober.host.c_str(), result.endpoint.text.c_str());
                m_stateDirty = true;
            }
            prober.endpoint = result.endpoint;
            prober.resolveFailures = 0;
            prober.lastError.clear();
            prober.nextResolveUs = now + kResolveTtlUs;
        } else {
            prober.resolveFailures = std::min(prober.resolveFailures + 1, 16);
            const uint64_t backoff = std::min(kResolveBackoffBaseUs << std::min(prober.resolveFailures - 1, 6), kResolveBackoffMaxUs);
            prober.nextResolveUs = now + backoff;
            prober.lastError = "DNS: " + result.error;
            NH_DEBUG("resolving %s failed: %s", prober.host.c_str(), result.error.c_str());
            m_stateDirty = true;
        }
    }
}

// ---------------------------------------------------------------------------
// State publishing
// ---------------------------------------------------------------------------

Json Monitor::buildStateJson(uint64_t now, bool verbose) const
{
    Json engine = Json::object();
    Json ipv4 = Json::object();
    ipv4.set("available", m_socket4.isOpen()).set("raw", m_socket4.isRaw()).set("error", m_error4);
    Json ipv6 = Json::object();
    ipv6.set("available", m_socket6.isOpen()).set("raw", m_socket6.isRaw()).set("error", m_error6);
    engine.set("ipv4", std::move(ipv4)).set("ipv6", std::move(ipv6));

    Json clients = Json::object();
    for (const Client &client : m_clients) {
        Json destinations = Json::array();
        size_t good = 0;
        size_t bad = 0;
        size_t unknown = 0;
        size_t enabled = 0;

        for (const Destination &destination : client.destinations) {
            const Prober *prober = (destination.prober >= 0 && size_t(destination.prober) < m_probers.size()) ? &m_probers[size_t(destination.prober)] : nullptr;

            if (destination.enabled) {
                ++enabled;
                switch (destination.state) {
                case HealthState::Good: ++good; break;
                case HealthState::Bad: ++bad; break;
                case HealthState::Unknown: ++unknown; break;
                }
            }

            const double loss = destination.sent > 0 ? (100.0 * double(destination.sent - destination.received) / double(destination.sent)) : 0.0;

            Json entry = Json::object();
            entry.set("id", destination.id)
                .set("state", healthStateName(destination.state))
                .set("resolved", prober && prober->endpoint.valid() ? prober->endpoint.text : std::string())
                .set("have_rtt", destination.haveRtt)
                .set("rtt_us", double(destination.rttUs))
                .set("sent", double(destination.sent))
                .set("received", double(destination.received))
                .set("loss_percent", loss)
                .set("error", prober ? prober->lastError : std::string());

            if (verbose) {
                entry.set("name", destination.name)
                    .set("address", destination.address)
                    .set("family", prober && prober->endpoint.valid() ? (prober->endpoint.family == AF_INET6 ? 6 : 4) : 0)
                    .set("enabled", destination.enabled)
                    .set("order", destination.order)
                    .set("threshold_us", double(destination.thresholdUs))
                    .set("sensitivity", destination.sensitivity)
                    .set("good_streak", destination.goodStreak)
                    .set("bad_streak", destination.badStreak)
                    .set("last_reply_us", double(destination.lastReplyRealUs))
                    .set("last_change_us", double(destination.lastChangeRealUs));
            }
            destinations.push(std::move(entry));
        }

        const char *overall = "unknown";
        if (enabled == 0) {
            overall = "idle";
        } else if (bad > 0) {
            overall = "bad";
        } else if (unknown == 0) {
            overall = "good";
        }

        Json counts = Json::object();
        counts.set("enabled", double(enabled))
            .set("good", double(good))
            .set("bad", double(bad))
            .set("unknown", double(unknown))
            .set("total", double(client.destinations.size()));

        Json entry = Json::object();
        entry.set("overall", overall).set("counts", std::move(counts));
        if (verbose) {
            entry.set("interval_ms", double(client.intervalUs / 1000))
                .set("timeout_ms", double(client.timeoutUs / 1000));
        }
        entry.set("destinations", std::move(destinations));
        clients.set(client.id, std::move(entry));
    }

    Json document = Json::object();
    document.set("version", 1)
        .set("generation", double(m_generation))
        .set("pid", double(::getpid()))
        .set("timestamp_us", double(nowRealtimeUs()))
        .set("probers", double(m_probers.size()))
        .set("engine", std::move(engine));
    if (verbose) {
        document.set("started_us", double(m_startedRealUs)).set("uptime_us", double(now));
    }
    document.set("clients", std::move(clients));

    return document;
}

uint64_t Monitor::publishIntervalUs() const
{
    return std::max(m_options.publishIntervalUs, m_minIntervalUs);
}

void Monitor::publish(uint64_t now, bool force)
{
    // A health change is published at once; a new round-trip time can wait for
    // the next slot, because the next probe has not been sent yet anyway.
    if (!force && !m_stateDirty) {
        if (!m_dataDirty) {
            return;
        }
        if (now - m_lastPublishUs < publishIntervalUs()) {
            return;
        }
    }

    ++m_generation;

    // What the widget reads. Pure ASCII inside the base64, so no character set
    // handling is involved anywhere between here and JSON.parse() in the widget.
    std::string ini = "[State]\n";
    ini += "generation=" + std::to_string(m_generation) + "\n";
    ini += "payload=" + base64Encode(buildStateJson(now, false).dump(-1, true)) + "\n";
    if (!writeFileAtomically(stateIniPath(), ini, 0644)) {
        NH_ERROR("could not write %s", stateIniPath().c_str());
    }

    // The same state laid out for a person to read. Nothing depends on it, so
    // it is refreshed on health changes and otherwise only now and then.
    if (m_stateDirty || m_lastReadablePublishUs == 0 || now - m_lastReadablePublishUs >= kReadablePublishIntervalUs) {
        if (!writeFileAtomically(stateFilePath(), buildStateJson(now, true).dump(2), 0644)) {
            NH_ERROR("could not write %s", stateFilePath().c_str());
        }
        m_lastReadablePublishUs = now;
    }

    m_lastPublishUs = now;
    m_dataDirty = false;
    m_stateDirty = false;
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

uint64_t Monitor::nextDeadline(uint64_t now) const
{
    uint64_t deadline = kNeverUs;
    for (const Prober &prober : m_probers) {
        if (prober.pending) {
            deadline = std::min(deadline, prober.pendingDeadlineUs);
        }
        deadline = std::min(deadline, prober.nextSendUs);
        if (!prober.numeric && !prober.resolving) {
            deadline = std::min(deadline, prober.nextResolveUs);
        }
    }
    if (m_dataDirty || m_stateDirty) {
        deadline = std::min(deadline, m_lastPublishUs + publishIntervalUs());
    }
    if (m_options.clientTimeoutUs != 0 && !m_clients.empty()) {
        deadline = std::min(deadline, m_lastClientCheckUs + clientCheckIntervalUs());
    }
    if (m_options.idleExitUs != 0 && m_idleSinceUs != 0) {
        deadline = std::min(deadline, m_idleSinceUs + m_options.idleExitUs);
    }
    (void)now;
    return deadline;
}

int Monitor::run()
{
    m_running = true;
    struct epoll_event events[8];

    while (m_running) {
        const uint64_t now = nowMonotonicUs();
        const uint64_t deadline = nextDeadline(now);

        int timeoutMs = -1;
        if (deadline != kNeverUs) {
            timeoutMs = (deadline <= now) ? 0 : int(std::min<uint64_t>((deadline - now + 999) / 1000, 3600000));
        }

        const int count = ::epoll_wait(m_epollFd, events, int(sizeof(events) / sizeof(events[0])), timeoutMs);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            NH_ERROR("epoll_wait: %s", std::strerror(errno));
            return 1;
        }

        const uint64_t after = nowMonotonicUs();
        bool socketReadable = false;

        for (int i = 0; i < count; ++i) {
            const int fd = events[i].data.fd;
            if (fd == m_signalFd) {
                struct signalfd_siginfo info {};
                while (::read(m_signalFd, &info, sizeof(info)) == sizeof(info)) {
                    if (info.ssi_signo == SIGHUP) {
                        NH_INFO("SIGHUP: reloading configuration");
                        m_configDirty = true;
                    } else {
                        NH_INFO("signal %u: shutting down", info.ssi_signo);
                        m_running = false;
                    }
                }
            } else if (fd == m_inotifyFd) {
                handleInotify();
            } else if (fd == m_resolver.notifyFd()) {
                handleResolutions(after);
            } else if (fd == m_socket4.fd() || fd == m_socket6.fd()) {
                socketReadable = true;
            }
        }

        if (!m_running) {
            break;
        }

        if (socketReadable) {
            handleReplies(after);
        }
        if (m_configDirty) {
            reloadClients();
        }

        tick(nowMonotonicUs());
        dropExpiredClients(after);

        if (m_probers.empty()) {
            if (m_idleSinceUs == 0) {
                m_idleSinceUs = after;
            }
            if (m_options.idleExitUs != 0 && after - m_idleSinceUs >= m_options.idleExitUs) {
                NH_INFO("nothing to monitor for %llus, exiting", (unsigned long long)(m_options.idleExitUs / 1000000));
                m_running = false;
            }
        } else {
            m_idleSinceUs = 0;
        }

        publish(nowMonotonicUs(), false);
    }

    publish(nowMonotonicUs(), true);
    return 0;
}

} // namespace nh
