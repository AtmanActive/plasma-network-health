// SPDX-License-Identifier: MIT
import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.SimpleKCM {
    id: page

    property alias cfg_intervalMs: intervalBox.value
    property alias cfg_timeoutMs: timeoutBox.value
    property alias cfg_defaultThresholdUs: defaultThresholdBox.value
    property alias cfg_defaultSensitivity: defaultSensitivityBox.value
    property alias cfg_daemonPath: daemonPathField.text

    readonly property string runtimeRoot: {
        const location = String(StandardPaths.writableLocation(StandardPaths.RuntimeLocation));
        return location.indexOf("file://") === 0 ? location.substring(7) : location;
    }
    readonly property string statePath: runtimeRoot + "/plasma-network-health/state.ini"
    readonly property string readableStatePath: runtimeRoot + "/plasma-network-health/state.json"

    Settings {
        id: stateFile
        location: "file://" + page.statePath
        category: "State"
    }

    property string backendStatus: i18n("Checking…")
    property bool backendHealthy: false

    function checkBackend() {
        stateFile.sync();
        const payload = String(stateFile.value("payload", ""));
        if (payload.length === 0) {
            backendHealthy = false;
            backendStatus = i18n("Not running");
            return;
        }
        try {
            const document = JSON.parse(Qt.atob(payload));
            const engine = document.engine || {};
            const ipv4 = (engine.ipv4 || {}).available === true;
            const ipv6 = (engine.ipv6 || {}).available === true;
            backendHealthy = ipv4 || ipv6;
            if (backendHealthy) {
                backendStatus = i18n("Running (pid %1), %2 probe stream(s), %3",
                                     String(document.pid),
                                     String(document.probers),
                                     ipv4 && ipv6 ? i18n("IPv4 and IPv6")
                                                  : (ipv4 ? i18n("IPv4 only") : i18n("IPv6 only")));
            } else {
                backendStatus = i18n("Running, but no ICMP socket could be opened. Check net.ipv4.ping_group_range.");
            }
        } catch (error) {
            backendHealthy = false;
            backendStatus = i18n("Not running");
        }
    }

    Component.onCompleted: checkBackend()

    Kirigami.FormLayout {
        anchors.fill: parent

        QQC2.SpinBox {
            id: intervalBox
            Kirigami.FormData.label: i18nc("@label:spinbox", "Check every:")
            from: 100
            to: 3600000
            stepSize: 100
            editable: true
            textFromValue: (value, locale) => i18ncp("@item:valuesuffix milliseconds", "%1 ms", "%1 ms", value)
            valueFromText: (text, locale) => parseInt(text, 10)
        }

        QQC2.SpinBox {
            id: timeoutBox
            Kirigami.FormData.label: i18nc("@label:spinbox", "Treat as lost after:")
            from: 50
            to: 60000
            stepSize: 100
            editable: true
            textFromValue: (value, locale) => i18ncp("@item:valuesuffix milliseconds", "%1 ms", "%1 ms", value)
            valueFromText: (text, locale) => parseInt(text, 10)
        }

        QQC2.Label {
            text: i18n("One probe is in flight per destination at a time. Probes that come due at the same moment are sent together, so the interval costs the same whether you monitor one destination or a hundred.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        QQC2.SpinBox {
            id: defaultThresholdBox
            Kirigami.FormData.label: i18nc("@label:spinbox", "New destinations use threshold:")
            from: 1
            to: 60000000
            stepSize: 500
            editable: true
            textFromValue: (value, locale) => i18ncp("@item:valuesuffix microseconds", "%1 µs", "%1 µs", value)
            valueFromText: (text, locale) => parseInt(text, 10)
        }

        QQC2.SpinBox {
            id: defaultSensitivityBox
            Kirigami.FormData.label: i18nc("@label:spinbox", "New destinations use sensitivity:")
            from: 0
            to: 1000
            editable: true
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label", "Backend:")

            Kirigami.Icon {
                source: page.backendHealthy ? "dialog-ok" : "dialog-warning"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }

            QQC2.Label {
                text: page.backendStatus
                wrapMode: Text.WordWrap
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            }

            QQC2.ToolButton {
                icon.name: "view-refresh"
                display: QQC2.AbstractButton.IconOnly
                onClicked: page.checkBackend()
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Check again")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        QQC2.TextField {
            id: daemonPathField
            Kirigami.FormData.label: i18nc("@label:textbox", "Backend executable:")
            placeholderText: i18nc("@info:placeholder", "Search the usual locations")
            Layout.preferredWidth: Kirigami.Units.gridUnit * 24
        }

        QQC2.Label {
            text: i18n("Current state is also written in readable form to <tt>%1</tt>.", page.readableStatePath)
            textFormat: Text.StyledText
            wrapMode: Text.WrapAnywhere
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }

        QQC2.Label {
            text: i18n("Leave empty unless plasma-network-healthd is installed somewhere unusual. The widget starts it automatically and it exits again once no widget needs it.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }
    }
}
