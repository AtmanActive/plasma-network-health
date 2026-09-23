// SPDX-License-Identifier: MIT
import QtQuick

import org.kde.kirigami as Kirigami

/// Three rings, one within the other: the system tray face of the widget.
Item {
    id: target

    property color ringColor: "#ffffff"
    readonly property int thickness: Math.max(1, Math.round(width * 0.115))

    implicitWidth: Kirigami.Units.iconSizes.smallMedium
    implicitHeight: implicitWidth

    Rectangle {
        anchors.centerIn: parent
        width: Math.round(target.width)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: target.thickness
        border.color: target.ringColor
        antialiasing: true
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.round(target.width * 0.62)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: target.thickness
        border.color: target.ringColor
        antialiasing: true
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.max(2, Math.round(target.width * 0.24))
        height: width
        radius: width / 2
        color: target.ringColor
        antialiasing: true
    }

    Behavior on ringColor {
        ColorAnimation {
            duration: Kirigami.Units.shortDuration
        }
    }
}
