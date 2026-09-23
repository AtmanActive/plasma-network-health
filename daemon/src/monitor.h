// SPDX-License-Identifier: MIT
#pragma once

#include "json.hpp"
#include "pinger.h"
#include "resolver.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nh {

enum class HealthState { Unknown = 0, Good = 1, Bad = 2 };

const char *healthStateName(HealthState state);

struct Destination {
    // -- configuration ----------------------------------------------------
    std::string id;
    std::string name;
    std::string address;
    int order = 0;
    uint64_t thresholdUs = 1000;
    int sensitivity = 3;
    bool enabled = true;

    // -- runtime ----------------------------------------------------------
    int prober = -1;
    HealthState state = HealthState::Unknown;
    int goodStreak = 0;
    int badStreak = 0;
    bool haveRtt = false;
    uint64_t rttUs = 0;
    uint64_t sent = 0;
    uint64_t received = 0;
    uint64_t lastReplyRealUs = 0;
    uint64_t lastChangeRealUs = 0;
};

struct Client {
    std::string id;
    std::string path;
    uint64_t intervalUs = 1000000;
    uint64_t timeoutUs = 1000000;
    std::vector<Destination> destinations;
    std::string error;
};

/// One ICMP probe stream. Destinations that share an address, interval and
/// timeout - including across plasmoid instances - share a single prober, so
/// the same host is never pinged twice.
struct Prober {
    std::string host;
    uint64_t intervalUs = 1000000;
    uint64_t timeoutUs = 1000000;
    uint32_t uid = 0;

    Endpoint endpoint;
    bool numeric = false;
    bool resolving = false;
    std::string lastError;
    uint64_t nextResolveUs = 0;
    int resolveFailures = 0;

    uint64_t nextSendUs = 0;
    bool pending = false;
    uint32_t pendingSeq = 0;
    uint64_t pendingDeadlineUs = 0;
    uint32_t sequence = 0;

    uint64_t sent = 0;
    uint64_t received = 0;

    struct Subscriber {
        size_t client;
        size_t destination;
    };
    std::vector<Subscriber> subscribers;
};

struct MonitorOptions {
    std::string runtimeDir;         ///< .../plasma-network-health
    uint64_t publishIntervalUs = 250000;
    uint64_t clientTimeoutUs = 900ull * 1000000ull; ///< drop clients whose config stopped being refreshed
    uint64_t idleExitUs = 0;        ///< 0 disables
    size_t maxDestinations = 128;
};

class Monitor {
public:
    explicit Monitor(MonitorOptions options);
    ~Monitor();

    bool start(std::string &error);
    int run();
    void requestStop() { m_running = false; }

    std::string stateFilePath() const { return m_options.runtimeDir + "/state.json"; }
    /// What the widget actually reads: QML can open this through QSettings,
    /// which it cannot do for a plain JSON file.
    std::string stateIniPath() const { return m_options.runtimeDir + "/state.ini"; }
    std::string clientsDirPath() const { return m_options.runtimeDir + "/clients"; }

private:
    bool openSockets();
    void reloadClients();
    bool loadClientFile(const std::string &path, const std::string &id, Client &out) const;
    void rebuildProbers();

    void tick(uint64_t now);
    void sendProbe(Prober &prober, uint64_t now);
    void applySample(Prober &prober, bool received, uint64_t rttUs, uint64_t now);
    void handleReplies(uint64_t now);
    void handleResolutions(uint64_t now);
    void handleInotify();
    void dropExpiredClients(uint64_t now);
    uint64_t clientCheckIntervalUs() const;

    uint64_t nextDeadline(uint64_t now) const;
    void publish(uint64_t now, bool force);
    /// Routine updates are never published faster than probes are sent, since
    /// nothing new can have happened in between.
    uint64_t publishIntervalUs() const;
    /// \param verbose include the fields that exist only for a human reading
    /// state.json. The widget's payload carries just what it renders.
    Json buildStateJson(uint64_t now, bool verbose) const;

    MonitorOptions m_options;

    IcmpSocket m_socket4;
    IcmpSocket m_socket6;
    std::string m_error4;
    std::string m_error6;

    Resolver m_resolver;

    int m_epollFd = -1;
    int m_inotifyFd = -1;
    int m_inotifyWatch = -1;
    int m_signalFd = -1;

    std::vector<Client> m_clients;
    std::vector<Prober> m_probers;
    std::unordered_map<uint32_t, size_t> m_proberByUid;
    uint32_t m_nextUid = 1;
    uint64_t m_minIntervalUs = 0; ///< fastest configured probe interval
    uint32_t m_cookie = 0;

    std::vector<Reply> m_replyScratch;

    bool m_running = false;
    bool m_configDirty = false;
    bool m_dataDirty = true;
    bool m_stateDirty = true;
    uint64_t m_lastPublishUs = 0;
    uint64_t m_lastReadablePublishUs = 0;
    uint64_t m_generation = 0;
    uint64_t m_startedRealUs = 0;
    uint64_t m_lastClientCheckUs = 0;
    uint64_t m_idleSinceUs = 0;
};

} // namespace nh
