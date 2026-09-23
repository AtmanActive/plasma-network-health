// SPDX-License-Identifier: MIT
//
// One setting, shared by every Network Health widget.
//
// A widget instance in Plasma owns its configuration, so the desktop widget and
// the system tray icon would otherwise each keep their own hosts and their own
// appearance. This keeps them in step through a small file in ~/.config, using
// the same reading trick as the state file: QSettings is the one local-file
// reader QML still has.
//
// One instance of this per thing being shared; the payload is opaque to it.
import QtCore
import QtQuick

import org.kde.plasma.plasma5support as P5Support


Item {
    id: store

    /// File under ~/.config/plasma-network-health, and the INI group inside it.
    property string fileName: "shared.ini"
    property string category: "Shared"

    /// Whether this widget takes part in the shared value at all.
    property bool active: true
    /// Set once the owner's configuration has actually been loaded. Until then
    /// localJson is still the empty placeholder from binding set-up, and acting
    /// on it would publish that emptiness over everyone else's list.
    property bool ready: false
    /// This widget's own value, in a canonical spelling so that two widgets can
    /// tell whether they hold the same thing by comparing strings.
    property string localJson: "[]"
    /// The shared revision this widget last agreed with.
    property int syncedRevision: 0
    /// What "nothing here" looks like for this payload. A value equal to it is
    /// never published over a non-empty shared one at start-up.
    property string emptyJson: "[]"

    /// The shared value changed elsewhere; take this as ours.
    signal adopt(string json)
    /// We are now in step with the shared value at this revision.
    signal synced(int revision)

    readonly property string directory: {
        const location = String(StandardPaths.writableLocation(StandardPaths.GenericConfigLocation));
        const base = location.indexOf("file://") === 0 ? location.substring(7) : location;
        return base + "/plasma-network-health";
    }
    readonly property string path: directory + "/" + fileName

    QtObject {
        id: priv
        /// Revision we have written but not yet seen come back.
        property int pendingRevision: 0
        property real pendingSince: 0
        /// The list as it was when we were last in step with the shared one.
        /// Differing from it is what makes a local edit an edit.
        property string baseline: ""
    }

    Settings {
        id: file
        location: "file://" + store.path
        category: store.category
    }

    P5Support.DataSource {
        id: shell
        engine: "executable"
        connectedSources: []
        onNewData: function (source, data) {
            disconnectSource(source);
        }
    }

    function shellQuote(value) {
        return "'" + String(value).split("'").join("'\\''") + "'";
    }

    function write(revision, json) {
        const body = "[" + category + "]\nrevision=" + revision + "\npayload=" + Qt.btoa(json) + "\n";
        const script = "umask 077\n"
                     + "mkdir -p " + shellQuote(directory) + " || exit 1\n"
                     + "printf '%s' " + shellQuote(Qt.btoa(body)) + " | base64 -d > " + shellQuote(path + ".new") + " || exit 1\n"
                     + "mv -f " + shellQuote(path + ".new") + " " + shellQuote(path) + "\n";
        shell.connectSource("sh -c 'echo " + Qt.btoa(script) + " | base64 -d | sh'");
    }

    function read() {
        file.sync();
        const revision = Number(file.value("revision", 0));
        const payload = String(file.value("payload", ""));
        // The payload is whatever the owner put in; it is already canonical.
        const json = payload.length > 0 ? Qt.atob(payload) : emptyJson;
        return {
            "revision": isFinite(revision) && revision > 0 ? revision : 0,
            "json": json
        };
    }

    function sync() {
        if (!active || !ready) {
            return;
        }

        const shared = read();

        if (priv.pendingRevision !== 0) {
            if (shared.revision >= priv.pendingRevision) {
                priv.pendingRevision = 0;
                synced(shared.revision);
                if (shared.json !== localJson) {
                    // Another widget wrote at the same moment and won; take its
                    // list rather than trading writes back and forth.
                    priv.baseline = shared.json;
                    adopt(shared.json);
                } else {
                    priv.baseline = localJson;
                }
            } else if (Date.now() - priv.pendingSince > 5000) {
                priv.pendingRevision = 0; // the write did not land, try again
            }
            return;
        }

        if (shared.json === localJson) {
            priv.baseline = localJson;
            if (syncedRevision !== shared.revision) {
                synced(shared.revision);
            }
            return;
        }

        if (shared.revision === 0) {
            // Nobody has published anything yet, so ours becomes the shared one.
            publish(1);
            return;
        }

        if (syncedRevision === shared.revision) {
            // The shared list has not moved since we last agreed with it, so the
            // difference is our edit and ours is the newer list.
            //
            // The exception is an empty list we have had since start-up: that is
            // far more likely to be a configuration that failed to load than a
            // deliberate "delete everything", and it would wipe every other
            // widget's list. Emptying the list while running still propagates,
            // because then it differs from the baseline.
            const wouldWipeSharedList = localJson === emptyJson && shared.json !== emptyJson && localJson === priv.baseline;
            if (!wouldWipeSharedList) {
                publish(shared.revision + 1);
                return;
            }
        }

        // Anything else means the shared list is the one that moved.
        priv.baseline = shared.json;
        adopt(shared.json);
        synced(shared.revision);
    }

    function publish(revision) {
        priv.pendingRevision = revision;
        priv.pendingSince = Date.now();
        write(revision, localJson);
    }

    onReadyChanged: {
        if (ready) {
            priv.baseline = localJson;
            debounce.restart();
        }
    }

    // Reacting to the change directly would write the configuration from inside
    // the binding that reads it, which QML rightly calls a loop.
    onLocalJsonChanged: if (ready) debounce.restart()
    onActiveChanged: if (ready) debounce.restart()

    Timer {
        id: debounce
        interval: 250
        onTriggered: store.sync()
    }

    Timer {
        interval: 2000
        repeat: true
        running: store.active && store.ready
        onTriggered: store.sync()
    }
}
