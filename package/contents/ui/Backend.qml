// SPDX-License-Identifier: MIT
//
// Talks to plasma-network-healthd.
//
// Everything expensive - raising ICMP packets, waiting for replies, timing out
// dead hosts - happens in that separate process. All this object ever does
// inside plasmashell is re-read one small file from the tmpfs-backed runtime
// directory, and hand the daemon a new config file when the user changes
// something. No network call is ever made from the Plasma process.
import QtCore
import QtQuick

import org.kde.plasma.plasma5support as P5Support

Item {
    id: backend

    // ---- configuration in -------------------------------------------------

    /// Stable identity of this widget instance; also the daemon's config file name.
    property string clientId: ""
    property int intervalMs: 1000
    property int timeoutMs: 1000
    /// Array of { id, name, address, order, thresholdUs, sensitivity, enabled }.
    property var destinations: []
    /// Optional explicit path to the daemon binary.
    property string daemonPath: ""
    /// Set to false to stop polling entirely (e.g. the widget is being removed).
    property bool active: true

    // ---- state out --------------------------------------------------------

    /// "idle" | "unknown" | "good" | "bad"
    property string overall: "idle"
    property bool daemonRunning: false
    /// Human readable reason why nothing is being monitored, or "".
    property string problem: ""
    property int generation: -1
    property real lastUpdateMs: 0

    readonly property alias model: destinationModel
    readonly property int goodCount: counters.good
    readonly property int badCount: counters.bad
    readonly property int unknownCount: counters.unknown
    readonly property int enabledCount: counters.enabled

    // ---- paths ------------------------------------------------------------

    readonly property string runtimeRoot: {
        const location = String(StandardPaths.writableLocation(StandardPaths.RuntimeLocation));
        if (location.length > 0) {
            return stripFileScheme(location);
        }
        // Same fallback QStandardPaths and the daemon use.
        const home = stripFileScheme(String(StandardPaths.writableLocation(StandardPaths.HomeLocation)));
        const user = home.substring(home.lastIndexOf("/") + 1);
        return "/tmp/runtime-" + user;
    }
    readonly property string baseDir: runtimeRoot + "/plasma-network-health"
    readonly property string clientsDir: baseDir + "/clients"
    readonly property string configPath: clientsDir + "/" + clientId + ".json"
    readonly property string statePath: baseDir + "/state.ini"
    readonly property string lockPath: baseDir + "/daemon.lock"

    // ---- internals --------------------------------------------------------

    readonly property int pollIntervalMs: Math.max(250, Math.min(intervalMs, 5000))

    /// Serialised in the daemon's own schema; changing it is what triggers a write.
    readonly property string daemonConfig: {
        const payload = {
            "version": 1,
            "client": clientId,
            "interval_ms": intervalMs,
            "timeout_ms": timeoutMs,
            "destinations": []
        };
        for (let i = 0; i < destinations.length; ++i) {
            const destination = destinations[i];
            payload.destinations.push({
                "id": destination.id,
                "name": destination.name,
                "address": destination.address,
                "order": destination.order,
                "threshold_us": destination.thresholdUs,
                "sensitivity": destination.sensitivity,
                "enabled": destination.enabled
            });
        }
        return JSON.stringify(payload);
    }

    QtObject {
        id: counters
        property int good: 0
        property int bad: 0
        property int unknown: 0
        property int enabled: 0
    }

    QtObject {
        id: priv
        property var queries: ({})
        property string writtenConfig: ""
        property real lastWriteMs: 0
        property real lastLaunchMs: 0
        property int missedPolls: 0
        property int staleTicks: 0
    }

    ListModel {
        id: destinationModel
    }

    /// QML cannot read a local file with XMLHttpRequest any more (Qt 6 disables
    /// it unless QML_XHR_ALLOW_FILE_READ is set for the whole process), so the
    /// daemon publishes an INI file and QSettings reads it. That turns out to be
    /// the cheaper path anyway: sync() only re-parses when the file actually
    /// changed, otherwise it costs a stat.
    Settings {
        id: stateFile
        location: "file://" + backend.statePath
        category: "State"
    }

    /// Fire-and-forget commands (writing the config file, starting the daemon).
    P5Support.DataSource {
        id: shell
        engine: "executable"
        connectedSources: []
        onNewData: function (source, data) {
            disconnectSource(source);
        }
    }

    /// Commands whose output we need back. Only used for one-off lookups such as
    /// finding the default gateway, never on a timer.
    P5Support.DataSource {
        id: query
        engine: "executable"
        connectedSources: []
        onNewData: function (source, data) {
            disconnectSource(source);
            const callback = priv.queries[source];
            if (callback === undefined) {
                return;
            }
            delete priv.queries[source];
            callback(String(data["stdout"] || "").trim(), data["exit code"]);
        }
    }

    // ---- shell helpers ----------------------------------------------------

    function stripFileScheme(url) {
        return url.indexOf("file://") === 0 ? url.substring(7) : url;
    }

    /// Single-quotes a value for POSIX sh.
    function shellQuote(value) {
        return "'" + String(value).split("'").join("'\\''") + "'";
    }

    /// Runs a script without any quoting hazards: the whole thing travels as
    /// base64 and is decoded by the shell itself.
    function runScript(script) {
        shell.connectSource("sh -c 'echo " + Qt.btoa(script) + " | base64 -d | sh'");
    }

    function runQuery(script, callback) {
        const command = "sh -c 'echo " + Qt.btoa(script) + " | base64 -d | sh'";
        priv.queries[command] = callback;
        query.connectSource(command);
    }

    /// Asks the kernel routing table for the default gateway. One fork, once.
    function detectGateway(callback) {
        runQuery("ip -4 route show default 2>/dev/null | awk '/default/ { print $3; exit }'\n", callback);
    }

    function writeConfig() {
        if (clientId.length === 0) {
            return;
        }
        priv.writtenConfig = daemonConfig;
        priv.lastWriteMs = Date.now();

        const directory = shellQuote(clientsDir);
        const target = shellQuote(configPath);
        const temporary = shellQuote(configPath + ".new");
        runScript("umask 077\n"
                + "mkdir -p " + directory + " || exit 1\n"
                + "printf '%s' " + shellQuote(Qt.btoa(daemonConfig)) + " | base64 -d > " + temporary + " || exit 1\n"
                + "mv -f " + temporary + " " + target + "\n");
    }

    function removeConfig() {
        if (clientId.length === 0) {
            return;
        }
        runScript("rm -f " + shellQuote(configPath) + "\n");
    }

    /// Starts the daemon if it is not running. Safe to call repeatedly: the
    /// daemon is single-instance and a second start is a no-op.
    function launchDaemon() {
        const now = Date.now();
        if (now - priv.lastLaunchMs < 15000) {
            return;
        }
        priv.lastLaunchMs = now;
        runScript(daemonLaunchScript());
    }

    function daemonLaunchScript() {
        let candidates = [];
        if (daemonPath.length > 0) {
            candidates.push(daemonPath);
        }
        candidates = candidates.concat([
            stripFileScheme(String(StandardPaths.writableLocation(StandardPaths.HomeLocation))) + "/.local/bin/plasma-network-healthd",
            "/usr/local/bin/plasma-network-healthd",
            "/usr/bin/plasma-network-healthd",
            "/usr/libexec/plasma-network-healthd"
        ]);

        let script = "";
        for (let i = 0; i < candidates.length; ++i) {
            script += "if [ -x " + shellQuote(candidates[i]) + " ]; then\n"
                    + "  exec " + shellQuote(candidates[i]) + " --daemon --idle-exit 900 --runtime-dir " + shellQuote(baseDir) + "\n"
                    + "fi\n";
        }
        script += "found=$(command -v plasma-network-healthd 2>/dev/null)\n"
                + "if [ -n \"$found\" ]; then\n"
                + "  exec \"$found\" --daemon --idle-exit 900 --runtime-dir " + shellQuote(baseDir) + "\n"
                + "fi\n"
                + "exit 127\n";
        return script;
    }

    /// Stops the current daemon and starts a fresh one. Offered as a context
    /// menu action; nothing in normal operation needs it.
    function restartDaemon() {
        priv.lastLaunchMs = 0;
        priv.writtenConfig = "";
        // The daemon's name is truncated in /proc, so go by the pid it recorded
        // in its lock file rather than matching on the process name.
        runScript("pid=$(cat " + shellQuote(lockPath) + " 2>/dev/null)\n"
                + "if [ -n \"$pid\" ] && [ -e \"/proc/$pid\" ]; then kill \"$pid\" 2>/dev/null; fi\n"
                + "sleep 1\n"
                + daemonLaunchScript());
    }

    // ---- model ------------------------------------------------------------

    function rebuildModel() {
        // Keep whatever we already know about a destination so that editing an
        // unrelated row does not blink every LED back to grey.
        const previous = {};
        for (let r = 0; r < destinationModel.count; ++r) {
            const row = destinationModel.get(r);
            previous[row.destinationId] = {
                "health": row.health,
                "rttUs": row.rttUs,
                "haveRtt": row.haveRtt,
                "resolved": row.resolved,
                "lossPercent": row.lossPercent,
                "sent": row.sent,
                "received": row.received,
                "message": row.message
            };
        }

        destinationModel.clear();
        const sorted = destinations.slice().sort(function (a, b) {
            return (a.order - b.order) || a.name.localeCompare(b.name);
        });

        for (let i = 0; i < sorted.length; ++i) {
            const destination = sorted[i];
            if (destination.enabled === false) {
                continue; // the daemon ignores it too
            }
            const carried = previous[destination.id] || {};
            destinationModel.append({
                "destinationId": destination.id,
                "name": destination.name,
                "address": destination.address,
                "order": destination.order,
                "thresholdUs": destination.thresholdUs,
                "sensitivity": destination.sensitivity,
                "enabled": destination.enabled,
                "health": carried.health !== undefined ? carried.health : "unknown",
                "resolved": carried.resolved !== undefined ? carried.resolved : "",
                "rttUs": carried.rttUs !== undefined ? carried.rttUs : 0,
                "haveRtt": carried.haveRtt !== undefined ? carried.haveRtt : false,
                "lossPercent": carried.lossPercent !== undefined ? carried.lossPercent : 0,
                "sent": carried.sent !== undefined ? carried.sent : 0,
                "received": carried.received !== undefined ? carried.received : 0,
                "message": carried.message !== undefined ? carried.message : ""
            });
        }
        recount();
    }

    /// Writes a role only when it actually changed, so that untouched rows do
    /// not emit dataChanged and repaint.
    function assign(index, row, key, value) {
        if (row[key] !== value) {
            destinationModel.setProperty(index, key, value);
        }
    }

    function recount() {
        let good = 0;
        let bad = 0;
        let unknown = 0;
        let enabled = 0;
        for (let r = 0; r < destinationModel.count; ++r) {
            const row = destinationModel.get(r);
            if (!row.enabled) {
                continue;
            }
            ++enabled;
            if (row.health === "good") {
                ++good;
            } else if (row.health === "bad") {
                ++bad;
            } else {
                ++unknown;
            }
        }
        counters.good = good;
        counters.bad = bad;
        counters.unknown = unknown;
        counters.enabled = enabled;
    }

    function clearLiveState(message) {
        for (let r = 0; r < destinationModel.count; ++r) {
            const row = destinationModel.get(r);
            assign(r, row, "health", "unknown");
            assign(r, row, "haveRtt", false);
            assign(r, row, "message", message);
        }
        recount();
        overall = destinationModel.count > 0 ? "unknown" : "idle";
    }

    // ---- polling ----------------------------------------------------------

    function applyDocument(document) {
        generation = document.generation !== undefined ? document.generation : -1;
        lastUpdateMs = Date.now();

        const engine = document.engine || {};
        const ipv4 = engine.ipv4 || {};
        const ipv6 = engine.ipv6 || {};
        if (!ipv4.available && !ipv6.available) {
            problem = i18n("The backend cannot open an ICMP socket: %1", String(ipv4.error || ipv6.error || ""));
        } else {
            problem = "";
        }

        const client = document.clients ? document.clients[clientId] : undefined;
        if (client === undefined) {
            // The daemon is alive but has not seen our configuration yet.
            if (Date.now() - priv.lastWriteMs > 5000) {
                writeConfig();
            }
            clearLiveState("");
            return;
        }

        const byId = {};
        for (let i = 0; i < client.destinations.length; ++i) {
            byId[client.destinations[i].id] = client.destinations[i];
        }

        for (let r = 0; r < destinationModel.count; ++r) {
            const row = destinationModel.get(r);
            const live = byId[row.destinationId];
            if (live === undefined) {
                assign(r, row, "health", "unknown");
                assign(r, row, "haveRtt", false);
                continue;
            }
            assign(r, row, "health", live.state);
            assign(r, row, "resolved", live.resolved || "");
            assign(r, row, "haveRtt", live.have_rtt === true);
            assign(r, row, "rttUs", live.rtt_us);
            assign(r, row, "lossPercent", live.loss_percent);
            assign(r, row, "sent", live.sent);
            assign(r, row, "received", live.received);
            assign(r, row, "message", live.error || "");
        }

        recount();
        overall = client.overall;

        // The daemon only knows what we last told it; if the user edited the
        // config while the daemon was down, push it again.
        if (priv.writtenConfig !== daemonConfig && Date.now() - priv.lastWriteMs > 2000) {
            writeConfig();
        }
    }

    function poll() {
        if (clientId.length === 0) {
            return;
        }

        stateFile.sync();
        const published = Number(stateFile.value("generation", -1));
        if (!isFinite(published) || published < 0) {
            onPollFailed();
            return;
        }

        if (published === generation) {
            // Nothing new was published. Either the daemon has nothing to say
            // (no destinations) or it died without cleaning up after itself.
            priv.staleTicks = Math.min(priv.staleTicks + 1, 1000);
            if (enabledCount > 0 && priv.staleTicks > 5) {
                onPollFailed();
            }
            return;
        }

        const payload = String(stateFile.value("payload", ""));
        if (payload.length === 0) {
            onPollFailed();
            return;
        }

        let document;
        try {
            document = JSON.parse(Qt.atob(payload));
        } catch (error) {
            onPollFailed();
            return;
        }

        priv.missedPolls = 0;
        priv.staleTicks = 0;
        daemonRunning = true;
        applyDocument(document);
    }

    function onPollFailed() {
        priv.missedPolls = Math.min(priv.missedPolls + 1, 1000);
        priv.staleTicks = 0;
        generation = -1;
        if (priv.missedPolls >= 2 && daemonRunning) {
            daemonRunning = false;
        }
        if (!daemonRunning) {
            problem = i18n("The monitoring backend is not running.");
            clearLiveState("");
            if (destinationModel.count > 0) {
                launchDaemon();
                // A freshly started daemon needs our configuration.
                priv.lastWriteMs = 0;
                priv.writtenConfig = "";
            }
        }
    }

    Timer {
        id: pollTimer
        interval: backend.pollIntervalMs
        repeat: true
        running: backend.active && backend.clientId.length > 0
        triggeredOnStart: true
        onTriggered: backend.poll()
    }

    /// The daemon forgets widgets whose config file stops being refreshed, which
    /// is how it recovers from a plasmashell crash. Refresh it well inside that
    /// window; once every five minutes costs nothing.
    Timer {
        id: heartbeatTimer
        interval: 300000
        repeat: true
        running: backend.active && backend.clientId.length > 0
        onTriggered: backend.writeConfig()
    }

    onDaemonConfigChanged: {
        rebuildModel();
        if (daemonConfig !== priv.writtenConfig) {
            writeConfig();
        }
    }

    Component.onCompleted: {
        rebuildModel();
        writeConfig();
        poll();
    }

    Component.onDestruction: {
        // Best effort: if this does not make it (plasmashell being killed), the
        // daemon drops us once the config file goes stale.
        removeConfig();
    }
}
