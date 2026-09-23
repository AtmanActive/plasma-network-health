// SPDX-License-Identifier: MIT
import QtQuick

import org.kde.kirigami as Kirigami

/// The round indicator shown next to every destination name.
Item {
    id: led

    property color ledColor: "#808080"
    property bool haloVisible: true

    implicitWidth: Kirigami.Units.iconSizes.small
    implicitHeight: implicitWidth

    Rectangle {
        anchors.centerIn: parent
        width: led.width
        height: width
        radius: width / 2
        color: led.ledColor
        opacity: led.haloVisible ? 0.2 : 0.0
        visible: opacity > 0
        antialiasing: true
    }

    Rectangle {
        id: lens
        anchors.centerIn: parent
        width: Math.round(led.width * 0.78)
        height: width
        radius: width / 2
        color: led.ledColor
        antialiasing: true
        border.width: Math.max(1, Math.round(width * 0.08))
        border.color: Qt.darker(led.ledColor, 1.8)

        Rectangle {
            x: Math.round(parent.width * 0.22)
            y: Math.round(parent.height * 0.15)
            width: Math.round(parent.width * 0.34)
            height: Math.round(width * 0.72)
            radius: Math.max(1, height / 2)
            color: Qt.rgba(1, 1, 1, 0.5)
            antialiasing: true
        }
    }

    Behavior on ledColor {
        ColorAnimation {
            duration: Kirigami.Units.shortDuration
        }
    }
}
