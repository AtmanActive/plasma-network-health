// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid

/// Panel and system tray face: one target board, coloured by overall health.
MouseArea {
    id: compact

    readonly property int squareSize: root.isVertical ? compact.width : compact.height

    Layout.minimumWidth: root.isVertical ? Kirigami.Units.iconSizes.small : Layout.minimumHeight
    Layout.minimumHeight: root.isVertical ? Layout.minimumWidth : Kirigami.Units.iconSizes.small
    Layout.preferredWidth: Layout.minimumWidth
    Layout.preferredHeight: Layout.minimumHeight

    activeFocusOnTab: true
    acceptedButtons: Qt.LeftButton
    hoverEnabled: true

    Accessible.role: Accessible.Button
    Accessible.name: i18n("Network Health")
    Accessible.description: root.statusSummary

    onClicked: root.expanded = !root.expanded
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.expanded = !root.expanded;
            event.accepted = true;
        }
    }

    TargetIcon {
        anchors.centerIn: parent
        width: Math.max(8, Math.round(Math.min(compact.width, compact.height) * 0.9))
        height: width
        ringColor: root.overallColor
    }
}
