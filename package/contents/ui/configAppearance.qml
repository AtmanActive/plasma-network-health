// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls

KCM.SimpleKCM {
    id: page

    property alias cfg_sharedAppearance: sharedBox.checked
    property alias cfg_showNames: showNamesBox.checked
    property alias cfg_showLatency: showLatencyBox.checked
    property alias cfg_ledSize: ledSizeBox.value
    property alias cfg_hideWhenHealthy: hideWhenHealthyBox.checked
    property alias cfg_colorGood: goodColorButton.color
    property alias cfg_colorBad: badColorButton.color
    property alias cfg_colorIdle: idleColorButton.color
    property int cfg_contentOpacity: 100
    property int cfg_backgroundOpacity: 100

    TextMetrics {
        id: percentMetrics
        font: Kirigami.Theme.defaultFont
        text: "100%"
    }

    Kirigami.FormLayout {
        anchors.fill: parent

        QQC2.CheckBox {
            id: sharedBox
            Kirigami.FormData.label: i18nc("@label", "Sharing:")
            text: i18nc("@option:check", "Use the same appearance in every Network Health widget")
        }

        QQC2.Label {
            text: sharedBox.checked
                  ? i18n("These settings are shared with your other Network Health widgets, so changing them here also changes the one on the desktop and the one in the system tray. The destination list has its own setting on the Destinations page.")
                  : i18n("These settings apply to this widget only. Turning sharing back on will replace them with the shared ones.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }

        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Indicators")
        }

        QQC2.SpinBox {
            id: ledSizeBox
            Kirigami.FormData.label: i18nc("@label:spinbox", "Size:")
            from: 6
            to: 64
            stepSize: 1
            editable: true
            textFromValue: (value, locale) => i18ncp("@item:valuesuffix pixels", "%1 pixel", "%1 pixels", value)
            valueFromText: (text, locale) => parseInt(text, 10)
        }

        QQC2.CheckBox {
            id: showNamesBox
            text: i18nc("@option:check", "Show destination names")
        }

        QQC2.CheckBox {
            id: showLatencyBox
            text: i18nc("@option:check", "Show round-trip time")
        }

        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Colours")
        }

        KQuickControls.ColorButton {
            id: goodColorButton
            Kirigami.FormData.label: i18nc("@label:chooser", "Healthy:")
        }

        KQuickControls.ColorButton {
            id: badColorButton
            Kirigami.FormData.label: i18nc("@label:chooser", "Unhealthy:")
        }

        KQuickControls.ColorButton {
            id: idleColorButton
            Kirigami.FormData.label: i18nc("@label:chooser", "Nothing to monitor:")
        }

        QQC2.Label {
            text: i18n("Used for the indicators and for the system tray icon, which takes the “nothing to monitor” colour when no destination is configured, the healthy colour when every destination is fine, and the unhealthy colour as soon as one is not.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }

        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "Desktop widget")
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:slider", "Contents opacity:")

            QQC2.Slider {
                id: opacitySlider
                from: 20
                to: 100
                stepSize: 5
                snapMode: QQC2.Slider.SnapAlways
                value: page.cfg_contentOpacity
                Layout.minimumWidth: Kirigami.Units.gridUnit * 12
                Layout.fillWidth: true
                // Only user interaction; assigning value programmatically must
                // not write the configuration back.
                onMoved: page.cfg_contentOpacity = Math.round(value)
            }

            QQC2.Label {
                text: i18nc("@info:status percentage", "%1%", Math.round(opacitySlider.value))
                horizontalAlignment: Text.AlignRight
                Layout.minimumWidth: percentMetrics.width
            }
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:slider", "Background opacity:")

            QQC2.Slider {
                id: backgroundSlider
                from: 0
                to: 100
                stepSize: 5
                snapMode: QQC2.Slider.SnapAlways
                value: page.cfg_backgroundOpacity
                Layout.minimumWidth: Kirigami.Units.gridUnit * 12
                Layout.fillWidth: true
                onMoved: page.cfg_backgroundOpacity = Math.round(value)
            }

            QQC2.Label {
                text: i18nc("@info:status percentage", "%1%", Math.round(backgroundSlider.value))
                horizontalAlignment: Text.AlignRight
                Layout.minimumWidth: percentMetrics.width
            }
        }

        QQC2.Label {
            text: i18n("Contents opacity fades the indicators and names, and stops at 20% so the widget can always be found again. Background opacity fades the panel behind them; at 0% there is none, which is the same as Plasma's “Hide Background”. Neither touches the system tray icon, and the list shown from the tray has the popup's own background.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }

        Item {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nc("@title:group", "System tray")
        }

        QQC2.CheckBox {
            id: hideWhenHealthyBox
            text: i18nc("@option:check", "Hide the icon while everything is healthy")
        }

        QQC2.Label {
            text: i18n("The icon moves into the tray's hidden section until a destination turns unhealthy.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }
    }
}
