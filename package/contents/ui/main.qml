// SPDX-License-Identifier: MIT
//
// Network Health - is my network still fine?
//
// The widget itself is deliberately dumb: it renders whatever the
// plasma-network-healthd backend last published and never performs any network
// operation of its own, so nothing the network does can stall plasmashell.
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid

import "utils.js" as Utils

PlasmoidItem {
    id: root

    readonly property bool isVertical: Plasmoid.formFactor === PlasmaCore.Types.Vertical

    readonly property color colorGood: Plasmoid.configuration.colorGood
    readonly property color colorBad: Plasmoid.configuration.colorBad
    readonly property color colorIdle: Plasmoid.configuration.colorIdle
    readonly property int ledSize: Plasmoid.configuration.ledSize
    /// Applies to the list of destinations, not to the panel or tray icon: a
    /// status light nobody can read is not a status light.
    readonly property real contentOpacity: Math.max(0.2, Math.min(1.0, Plasmoid.configuration.contentOpacity / 100))
    /// The widget background, which this widget draws itself so that it can be
    /// faded rather than only switched on and off.
    readonly property real backgroundOpacity: Math.max(0.0, Math.min(1.0, Plasmoid.configuration.backgroundOpacity / 100))
    /// Below full opacity the widget takes the background over from Plasma.
    readonly property bool drawsOwnBackground: Plasmoid.configuration.backgroundOpacity < 100
    readonly property bool showNames: Plasmoid.configuration.showNames
    readonly property bool showLatency: Plasmoid.configuration.showLatency

    readonly property alias backend: backendItem

    /// Plasmoid.id is unique per applet instance and survives restarts; the
    /// generated fallback only matters if a Plasma version ever stops exposing it.
    readonly property string clientId: {
        const appletId = Plasmoid.id;
        if (appletId !== undefined && appletId !== null && appletId > 0) {
            return "applet-" + appletId;
        }
        return Plasmoid.configuration.instanceId;
    }

    readonly property var destinationList: {
        try {
            const parsed = JSON.parse(Plasmoid.configuration.destinations);
            return Array.isArray(parsed) ? parsed : [];
        } catch (error) {
            return [];
        }
    }

    /// A stable spelling of our list, for comparison with the shared one.
    readonly property string canonicalDestinations: Utils.canonicalDestinations(destinationList)

    /// Mirrored into plain properties so change handlers can react to them;
    /// Plasmoid.configuration is a property map and does not offer them.
    readonly property bool sharingDestinations: Plasmoid.configuration.sharedDestinations
    readonly property bool sharingAppearance: Plasmoid.configuration.sharedAppearance

    /// The appearance every widget agrees on, in a comparable spelling.
    readonly property string canonicalAppearance: Utils.canonicalAppearance({
        "ledSize": Plasmoid.configuration.ledSize,
        "showNames": Plasmoid.configuration.showNames,
        "showLatency": Plasmoid.configuration.showLatency,
        "contentOpacity": Plasmoid.configuration.contentOpacity,
        "backgroundOpacity": Plasmoid.configuration.backgroundOpacity,
        "colorGood": Plasmoid.configuration.colorGood,
        "colorBad": Plasmoid.configuration.colorBad,
        "colorIdle": Plasmoid.configuration.colorIdle,
        "hideWhenHealthy": Plasmoid.configuration.hideWhenHealthy
    })

    /// Takes the shared appearance as this widget's own.
    function applyAppearance(text) {
        let shared;
        try {
            shared = JSON.parse(text);
        } catch (error) {
            return;
        }
        if (!shared || typeof shared !== "object") {
            return;
        }
        const configuration = Plasmoid.configuration;
        if (shared.ledSize !== undefined) {
            configuration.ledSize = shared.ledSize;
        }
        if (shared.showNames !== undefined) {
            configuration.showNames = shared.showNames;
        }
        if (shared.showLatency !== undefined) {
            configuration.showLatency = shared.showLatency;
        }
        if (shared.contentOpacity !== undefined) {
            configuration.contentOpacity = shared.contentOpacity;
        }
        if (shared.backgroundOpacity !== undefined) {
            configuration.backgroundOpacity = shared.backgroundOpacity;
        }
        if (shared.colorGood !== undefined) {
            configuration.colorGood = shared.colorGood;
        }
        if (shared.colorBad !== undefined) {
            configuration.colorBad = shared.colorBad;
        }
        if (shared.colorIdle !== undefined) {
            configuration.colorIdle = shared.colorIdle;
        }
        if (shared.hideWhenHealthy !== undefined) {
            configuration.hideWhenHealthy = shared.hideWhenHealthy;
        }
    }

    readonly property color overallColor: Utils.colorForState(backendItem.overall, colorGood, colorBad, colorIdle)

    readonly property string statusSummary: {
        if (!backendItem.daemonRunning) {
            return i18n("Monitoring backend is not running");
        }
        if (backendItem.enabledCount === 0) {
            return i18n("Nothing to monitor");
        }
        if (backendItem.badCount > 0) {
            return i18np("%1 destination is unhealthy", "%1 destinations are unhealthy", backendItem.badCount);
        }
        if (backendItem.unknownCount > 0) {
            return i18np("Waiting for %1 destination", "Waiting for %1 destinations", backendItem.unknownCount);
        }
        return i18np("The only destination is healthy", "All %1 destinations are healthy", backendItem.enabledCount);
    }

    Plasmoid.icon: "network-connect"
    Plasmoid.title: i18n("Network Health")

    // Plasma can draw its applet background or not draw it; it cannot fade it.
    // So at full opacity Plasma draws the real thing, and at anything less the
    // widget turns Plasma's background off and paints the same frame itself at
    // the chosen opacity. One slider, and Plasma's own switch kept in step with
    // it automatically.
    readonly property int wantedBackgroundHints: drawsOwnBackground
        ? PlasmaCore.Types.NoBackground
        : PlasmaCore.Types.StandardBackground

    Plasmoid.backgroundHints: wantedBackgroundHints

    // backgroundHints is what the widget asks for; userBackgroundHints is the
    // per-instance override Plasma stores and prefers. Both are set, so it does
    // not matter which one Plasma consults - and a stale override left by an
    // earlier version cannot keep an opaque background pinned underneath.
    Binding {
        target: Plasmoid
        property: "userBackgroundHints"
        value: root.wantedBackgroundHints
        restoreMode: Binding.RestoreNone
    }

    Plasmoid.status: {
        if (backendItem.badCount > 0) {
            return PlasmaCore.Types.NeedsAttentionStatus;
        }
        if (Plasmoid.configuration.hideWhenHealthy && backendItem.enabledCount > 0) {
            return PlasmaCore.Types.PassiveStatus;
        }
        return PlasmaCore.Types.ActiveStatus;
    }

    // Suppress Plasma's generated tooltip in favour of the custom one.
    toolTipMainText: ""
    toolTipSubText: ""
    toolTipItem: SummaryTooltip {}

    compactRepresentation: CompactRepresentation {}
    fullRepresentation: FullRepresentation {}

    Backend {
        id: backendItem

        clientId: root.clientId
        intervalMs: Plasmoid.configuration.intervalMs
        timeoutMs: Plasmoid.configuration.timeoutMs
        daemonPath: Plasmoid.configuration.daemonPath
        destinations: root.destinationList
    }

    SharedStore {
        id: sharedStore

        fileName: "destinations.ini"
        category: "Destinations"
        emptyJson: "[]"

        active: root.sharingDestinations
        localJson: root.canonicalDestinations
        syncedRevision: Plasmoid.configuration.sharedRevision

        onAdopt: function (json) {
            Plasmoid.configuration.destinations = json;
        }
        onSynced: function (revision) {
            Plasmoid.configuration.sharedRevision = revision;
        }
    }

    SharedStore {
        id: sharedAppearanceStore

        fileName: "appearance.ini"
        category: "Appearance"
        emptyJson: "{}"

        active: root.sharingAppearance
        localJson: root.canonicalAppearance
        syncedRevision: Plasmoid.configuration.sharedAppearanceRevision

        onAdopt: function (json) {
            root.applyAppearance(json);
        }
        onSynced: function (revision) {
            Plasmoid.configuration.sharedAppearanceRevision = revision;
        }
    }

    onSharingDestinationsChanged: {
        if (!sharingDestinations) {
            // Forget where we were, so that switching sharing back on joins the
            // shared list rather than overwriting it with our stale copy.
            Plasmoid.configuration.sharedRevision = 0;
        }
    }

    onSharingAppearanceChanged: {
        if (!sharingAppearance) {
            Plasmoid.configuration.sharedAppearanceRevision = 0;
        }
    }

    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18nc("@action", "Restart Monitoring Backend")
            icon.name: "system-reboot"
            onTriggered: backendItem.restartDaemon()
        }
    ]

    /// First run only: offer something useful to look at instead of an empty
    /// widget, using the one host every desktop is guaranteed to have.
    function seedDefaultDestination() {
        backendItem.detectGateway(function (address) {
            Plasmoid.configuration.seeded = true;
            if (!address || address.length === 0) {
                return;
            }
            Plasmoid.configuration.destinations = JSON.stringify([{
                "id": Utils.newId(),
                "name": i18n("Gateway"),
                "address": address,
                "order": 1,
                "thresholdUs": Plasmoid.configuration.defaultThresholdUs,
                "sensitivity": Plasmoid.configuration.defaultSensitivity,
                "enabled": true
            }]);
        });
    }

    Component.onCompleted: {
        if (!Plasmoid.configuration.instanceId) {
            Plasmoid.configuration.instanceId = Utils.newId();
        }
        // Skip the first-run gateway if a shared list already exists: this
        // widget is about to adopt that instead.
        const joiningSharedList = sharingDestinations && sharedStore.read().revision > 0;
        if (!Plasmoid.configuration.seeded && destinationList.length === 0 && !joiningSharedList) {
            seedDefaultDestination();
        }

        // The configuration is now genuinely loaded, so the shared stores may act.
        sharedStore.ready = true;
        sharedAppearanceStore.ready = true;
    }
}
