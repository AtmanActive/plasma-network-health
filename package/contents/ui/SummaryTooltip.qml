// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras

import "utils.js" as Utils

/// Tooltip shown for the panel / system tray icon.
ColumnLayout {
    id: summary

    readonly property int maximumRows: 12

    Layout.minimumWidth: Kirigami.Units.gridUnit * 14
    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
    spacing: Kirigami.Units.smallSpacing

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing * 2

        TargetIcon {
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
            ringColor: root.overallColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            PlasmaExtras.Heading {
                level: 4
                text: i18n("Network Health")
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            PlasmaComponents.Label {
                text: root.statusSummary
                opacity: 0.75
                font: Kirigami.Theme.smallFont
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }

    Kirigami.Separator {
        Layout.fillWidth: true
        visible: root.backend.model.count > 0
    }

    Repeater {
        model: root.backend.model

        delegate: RowLayout {
            required property int index
            required property string name
            required property string health
            required property bool haveRtt
            required property real rttUs

            // Long lists are truncated so the tooltip stays a tooltip.
            visible: index < summary.maximumRows
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing * 2

            StatusLed {
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
                haloVisible: false
                ledColor: Utils.colorForState(health, root.colorGood, root.colorBad, root.colorIdle)
            }

            PlasmaComponents.Label {
                text: name
                elide: Text.ElideRight
                maximumLineCount: 1
                Layout.fillWidth: true
            }

            PlasmaComponents.Label {
                text: Utils.formatRtt(rttUs, haveRtt)
                opacity: 0.7
                font: Kirigami.Theme.smallFont
            }
        }
    }

    PlasmaComponents.Label {
        visible: root.backend.model.count > summary.maximumRows
        text: i18np("and %1 more destination", "and %1 more destinations",
                    root.backend.model.count - summary.maximumRows)
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        Layout.fillWidth: true
    }

    PlasmaComponents.Label {
        visible: root.backend.problem.length > 0
        text: root.backend.problem
        color: Kirigami.Theme.negativeTextColor
        wrapMode: Text.WordWrap
        font: Kirigami.Theme.smallFont
        Layout.fillWidth: true
    }
}
