// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.ksvg as KSvg
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid

import "utils.js" as Utils

/// Desktop face: one row per destination - LED, then name.
Item {
    id: full

    /// In a panel the full representation only ever appears inside a popup,
    /// which brings its own background; anywhere else (the desktop, or a
    /// standalone host) we draw the widget background ourselves.
    readonly property bool inPopup: Plasmoid.formFactor === PlasmaCore.Types.Horizontal
                                 || Plasmoid.formFactor === PlasmaCore.Types.Vertical

    readonly property int rowHeight: Math.max(Kirigami.Units.gridUnit + Kirigami.Units.smallSpacing,
                                              root.ledSize + Kirigami.Units.smallSpacing * 2)
    readonly property int contentWidth: Math.max(Kirigami.Units.gridUnit * 9, longestRow + Kirigami.Units.gridUnit * 2)
    property int longestRow: Kirigami.Units.gridUnit * 6

    readonly property int frameLeft: background.visible ? background.margins.left : 0
    readonly property int frameRight: background.visible ? background.margins.right : 0
    readonly property int frameTop: background.visible ? background.margins.top : 0
    readonly property int frameBottom: background.visible ? background.margins.bottom : 0

    Layout.minimumWidth: Kirigami.Units.gridUnit * 8 + frameLeft + frameRight
    Layout.minimumHeight: rowHeight * 2 + frameTop + frameBottom
    Layout.preferredWidth: contentWidth + frameLeft + frameRight
    Layout.preferredHeight: frameTop + frameBottom
                          + Math.max(rowHeight * 2, Math.min(rowHeight * Math.max(list.count, 1), Kirigami.Units.gridUnit * 28))

    // The Plasma theme's own widget background, drawn here rather than by the
    // containment so that its opacity can be dialled.
    KSvg.FrameSvgItem {
        id: background

        anchors.fill: parent
        imagePath: "widgets/background"
        opacity: root.backgroundOpacity
        // Only below full opacity: at 100% Plasma is drawing the real one and a
        // second copy would sit on top of it.
        visible: root.drawsOwnBackground && opacity > 0 && !full.inPopup
    }

    Item {
        id: content

        anchors.fill: parent
        anchors.leftMargin: full.frameLeft
        anchors.rightMargin: full.frameRight
        anchors.topMargin: full.frameTop
        anchors.bottomMargin: full.frameBottom
        // Deliberately separate from the background's opacity: the two are
        // dialled independently.
        opacity: root.contentOpacity

        PlasmaComponents.ScrollView {
            id: scrollView
            anchors.fill: parent
            visible: list.count > 0
            contentWidth: availableWidth

            ListView {
                id: list
                model: root.backend.model
                clip: true
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    id: row

                    required property int index
                    required property string destinationId
                    required property string name
                    required property string address
                    required property string resolved
                    required property string health
                    required property bool haveRtt
                    required property real rttUs
                    required property real lossPercent
                    required property real thresholdUs
                    required property int sensitivity
                    required property string message

                    width: ListView.view.width
                    height: full.rowHeight

                    readonly property color stateColor: Utils.colorForState(health, root.colorGood, root.colorBad, root.colorIdle)
                    readonly property string stateWord: health === "good" ? i18nc("@info network destination is healthy", "Good")
                                                      : health === "bad" ? i18nc("@info network destination is unhealthy", "Bad")
                                                      : i18nc("@info network destination has not been measured yet", "Unknown")

                    Rectangle {
                        anchors.fill: parent
                        radius: Kirigami.Units.cornerRadius
                        color: Kirigami.Theme.highlightColor
                        opacity: hover.hovered ? 0.15 : 0.0
                        Behavior on opacity {
                            NumberAnimation {
                                duration: Kirigami.Units.shortDuration
                            }
                        }
                    }

                    HoverHandler {
                        id: hover
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Kirigami.Units.smallSpacing
                        anchors.rightMargin: Kirigami.Units.smallSpacing
                        spacing: Kirigami.Units.smallSpacing * 2

                        StatusLed {
                            implicitWidth: root.ledSize
                            implicitHeight: root.ledSize
                            ledColor: row.stateColor
                            Layout.alignment: Qt.AlignVCenter
                        }

                        PlasmaComponents.Label {
                            id: nameLabel
                            text: row.name
                            visible: root.showNames
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            onImplicitWidthChanged: full.longestRow = Math.max(full.longestRow, implicitWidth + root.ledSize)
                        }

                        Item {
                            Layout.fillWidth: true
                            visible: !root.showNames
                        }

                        PlasmaComponents.Label {
                            text: Utils.formatRtt(row.rttUs, row.haveRtt)
                            visible: root.showLatency
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            horizontalAlignment: Text.AlignRight
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    PlasmaCore.ToolTipArea {
                        anchors.fill: parent
                        // Status first, then the name, address and round-trip time.
                        mainText: row.name
                        textFormat: Text.StyledText
                        subText: {
                            const lines = [];
                            lines.push(i18nc("@info:tooltip", "Status: <b><font color=\"%1\">%2</font></b>",
                                             String(row.stateColor), row.stateWord));
                            const shown = row.resolved.length > 0 && row.resolved !== row.address
                                        ? Utils.escapeHtml(row.address) + " (" + Utils.escapeHtml(row.resolved) + ")"
                                        : Utils.escapeHtml(row.address);
                            lines.push(i18nc("@info:tooltip", "Address: %1", shown));
                            lines.push(i18nc("@info:tooltip", "Ping: %1", Utils.formatRtt(row.rttUs, row.haveRtt)));
                            lines.push(i18nc("@info:tooltip", "Threshold: %1, sensitivity: %2",
                                             Utils.formatThreshold(row.thresholdUs), String(row.sensitivity)));
                            lines.push(i18nc("@info:tooltip", "Packet loss: %1%", row.lossPercent.toFixed(1)));
                            if (row.message.length > 0) {
                                lines.push("<i>" + Utils.escapeHtml(row.message) + "</i>");
                            }
                            return lines.join("<br/>");
                        }
                    }
                }
            }
        }

        Loader {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 2
            active: list.count === 0
            visible: active
            asynchronous: true
            sourceComponent: PlasmaExtras.PlaceholderMessage {
                width: parent.width
                // Nothing configured is the common case and not an error, so say
                // that even when the backend has not been started yet.
                readonly property bool nothingConfigured: root.destinationList.length === 0
                readonly property bool allDisabled: !nothingConfigured && root.backend.model.count === 0

                iconName: nothingConfigured || allDisabled ? "network-server-symbolic" : "network-disconnect-symbolic"
                text: nothingConfigured ? i18n("No destinations configured")
                    : allDisabled ? i18n("Every destination is switched off")
                    : i18n("Monitoring backend unavailable")
                explanation: nothingConfigured ? i18n("Add the hosts you want to keep an eye on.")
                           : allDisabled ? i18n("Switch at least one back on to see its status.")
                           : root.backend.problem
                helpfulAction: Kirigami.Action {
                    text: i18nc("@action:button", "Configure…")
                    icon.name: "configure"
                    onTriggered: Plasmoid.internalAction("configure")?.trigger()
                }
            }
        }
    }
}
