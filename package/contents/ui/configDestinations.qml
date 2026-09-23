// SPDX-License-Identifier: MIT
import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as QtDialogs
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

import "utils.js" as Utils

KCM.SimpleKCM {
    id: page

    property string cfg_destinations
    property int cfg_defaultThresholdUs
    property int cfg_defaultSensitivity
    property alias cfg_sharedDestinations: sharedBox.checked

    readonly property int maximumDestinations: 128

    // Fixed widths so the header labels line up with the fields below them.
    readonly property int reorderColumnWidth: Kirigami.Units.gridUnit * 4
    readonly property int toggleColumnWidth: Kirigami.Units.gridUnit * 2
    readonly property int thresholdColumnWidth: Kirigami.Units.gridUnit * 8
    readonly property int sensitivityColumnWidth: Kirigami.Units.gridUnit * 6
    readonly property int removeColumnWidth: Kirigami.Units.gridUnit * 2

    /// Last value this page wrote, so that saving does not bounce back in as an
    /// external change and rebuild the rows while the user is typing.
    property string lastSerialised: ""

    /// Outcome of the last import or export, shown in a message above the list.
    property string noticeText: ""
    property int noticeType: Kirigami.MessageType.Information
    /// The list as it was before the last import, for the Undo action.
    property string undoJson: ""

    ListModel {
        id: rows
    }

    function load() {
        rows.clear();
        let parsed = [];
        try {
            const value = JSON.parse(cfg_destinations);
            if (Array.isArray(value)) {
                parsed = value;
            }
        } catch (error) {
            parsed = [];
        }

        parsed.sort(function (a, b) {
            return (a.order - b.order) || String(a.name).localeCompare(String(b.name));
        });

        for (let i = 0; i < parsed.length && i < maximumDestinations; ++i) {
            const entry = parsed[i];
            rows.append({
                "destinationId": entry.id !== undefined ? String(entry.id) : Utils.newId(),
                "name": entry.name !== undefined ? String(entry.name) : "",
                "address": entry.address !== undefined ? String(entry.address) : "",
                "thresholdUs": entry.thresholdUs !== undefined ? Number(entry.thresholdUs) : cfg_defaultThresholdUs,
                "sensitivity": entry.sensitivity !== undefined ? Number(entry.sensitivity) : cfg_defaultSensitivity,
                "destinationEnabled": entry.enabled !== false
            });
        }
    }

    function currentList() {
        const list = [];
        for (let i = 0; i < rows.count; ++i) {
            const row = rows.get(i);
            list.push({
                "id": row.destinationId,
                "name": row.name,
                "address": row.address.trim(),
                "order": i + 1,
                "thresholdUs": row.thresholdUs,
                "sensitivity": row.sensitivity,
                "enabled": row.destinationEnabled
            });
        }
        return list;
    }

    function replaceList(list) {
        rows.clear();
        for (let i = 0; i < list.length; ++i) {
            const entry = list[i];
            rows.append({
                "destinationId": entry.id,
                "name": entry.name,
                "address": entry.address,
                "thresholdUs": entry.thresholdUs,
                "sensitivity": entry.sensitivity,
                "destinationEnabled": entry.enabled !== false
            });
        }
        save();
    }

    function notify(type, text) {
        noticeType = type;
        noticeText = text;
        // Set rather than bound: the close button assigns visible itself, which
        // would break a binding for good.
        notice.visible = text.length > 0;
    }

    function save() {
        lastSerialised = JSON.stringify(currentList());
        cfg_destinations = lastSerialised;
    }

    function addDestination() {
        if (rows.count >= maximumDestinations) {
            return;
        }
        rows.append({
            "destinationId": Utils.newId(),
            "name": i18n("Destination %1", rows.count + 1),
            "address": "",
            "thresholdUs": cfg_defaultThresholdUs,
            "sensitivity": cfg_defaultSensitivity,
            "destinationEnabled": true
        });
        save();
    }

    function moveRow(from, to) {
        if (to < 0 || to >= rows.count) {
            return;
        }
        rows.move(from, to, 1);
        save();
    }

    onCfg_destinationsChanged: {
        if (cfg_destinations !== lastSerialised) {
            load();
        }
    }

    Component.onCompleted: load()

    FileAccess {
        id: fileAccess

        onWriteFinished: function (path, ok, error) {
            if (ok) {
                page.notify(Kirigami.MessageType.Positive,
                            i18np("Exported one destination to %2.", "Exported %1 destinations to %2.",
                                  rows.count, path));
            } else {
                page.notify(Kirigami.MessageType.Error, i18n("Could not export: %1", error));
            }
        }

        onReadFinished: function (path, ok, text, error) {
            if (!ok) {
                page.notify(Kirigami.MessageType.Error, i18n("Could not import: %1", error));
                return;
            }

            const result = Utils.importDocument(text, page.cfg_defaultThresholdUs,
                                                page.cfg_defaultSensitivity, page.maximumDestinations);
            if (!result.ok) {
                page.notify(Kirigami.MessageType.Error,
                            i18n("%1 does not contain a destination list.", path));
                return;
            }
            if (result.destinations.length === 0) {
                page.notify(Kirigami.MessageType.Error,
                            i18n("%1 contains no usable destinations.", path));
                return;
            }

            page.undoJson = JSON.stringify(page.currentList());
            page.replaceList(result.destinations);

            let message = i18np("Imported one destination.", "Imported %1 destinations.",
                                result.destinations.length);
            if (result.skipped > 0) {
                message += " " + i18np("One entry without an address was ignored.",
                                       "%1 entries without an address were ignored.", result.skipped);
            }
            if (result.dropped > 0) {
                message += " " + i18np("One entry past the limit of %2 was ignored.",
                                       "%1 entries past the limit of %2 were ignored.",
                                       result.dropped, page.maximumDestinations);
            }
            message += " " + i18n("Select Apply to keep them.");
            page.notify(Kirigami.MessageType.Positive, message);
        }
    }

    QtDialogs.FileDialog {
        id: exportDialog
        title: i18nc("@title:window", "Export Destinations")
        fileMode: QtDialogs.FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [i18n("Destination lists (*.json)"), i18n("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        selectedFile: currentFolder + "/network-health-destinations.json"
        onAccepted: fileAccess.write(String(selectedFile).replace("file://", ""),
                                     Utils.exportDocument(page.currentList()))
    }

    QtDialogs.FileDialog {
        id: importDialog
        title: i18nc("@title:window", "Import Destinations")
        fileMode: QtDialogs.FileDialog.OpenFile
        nameFilters: [i18n("Destination lists (*.json)"), i18n("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: fileAccess.read(String(selectedFile).replace("file://", ""))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18np("%1 destination", "%1 destinations", rows.count)
                opacity: 0.7
            }

            Item {
                Layout.fillWidth: true
            }

            QQC2.Button {
                text: i18nc("@action:button", "Add Destination")
                icon.name: "list-add"
                enabled: rows.count < page.maximumDestinations
                onClicked: page.addDestination()
            }

            QQC2.Button {
                text: i18nc("@action:button", "Export…")
                icon.name: "document-export"
                enabled: rows.count > 0
                onClicked: exportDialog.open()
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Save this list to a file")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }

            QQC2.Button {
                text: i18nc("@action:button", "Import…")
                icon.name: "document-import"
                onClicked: importDialog.open()
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Replace this list with one from a file")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        Kirigami.InlineMessage {
            id: notice

            Layout.fillWidth: true
            visible: false
            type: page.noticeType
            text: page.noticeText
            showCloseButton: true

            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Undo")
                    icon.name: "edit-undo"
                    visible: page.undoJson.length > 0
                    onTriggered: {
                        page.replaceList(Utils.parseDestinations(page.undoJson));
                        page.undoJson = "";
                        page.notify(Kirigami.MessageType.Information, i18n("The previous list was restored."));
                    }
                }
            ]
        }

        QQC2.CheckBox {
            id: sharedBox
            Layout.fillWidth: true
            text: i18nc("@option:check", "Use the same destinations in every Network Health widget")
        }

        QQC2.Label {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.gridUnit
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            text: sharedBox.checked
                  ? i18n("This list is shared with your other Network Health widgets — the one on the desktop and the one in the system tray stay in step. Size, colours and the other appearance settings stay separate for each widget.")
                  : i18n("This widget keeps its own list. Turning sharing back on will replace it with the shared one.")
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: rows.count >= page.maximumDestinations
            type: Kirigami.MessageType.Information
            text: i18n("The maximum of %1 destinations has been reached.", page.maximumDestinations)
        }

        // Column headers
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: rows.count > 0

            Item {
                Layout.preferredWidth: page.reorderColumnWidth
            }
            QQC2.Label {
                text: i18nc("@title:column", "On")
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                Layout.preferredWidth: page.toggleColumnWidth
                horizontalAlignment: Text.AlignHCenter
            }
            QQC2.Label {
                text: i18nc("@title:column", "Name")
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                Layout.fillWidth: true
                Layout.preferredWidth: 1
            }
            QQC2.Label {
                text: i18nc("@title:column", "Address")
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                Layout.fillWidth: true
                Layout.preferredWidth: 1
            }
            QQC2.Label {
                text: i18nc("@title:column round-trip time threshold in microseconds", "Threshold (µs)")
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                Layout.preferredWidth: page.thresholdColumnWidth
            }
            QQC2.Label {
                text: i18nc("@title:column", "Sensitivity")
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                Layout.preferredWidth: page.sensitivityColumnWidth
            }
            Item {
                Layout.preferredWidth: page.removeColumnWidth
            }
        }

        Repeater {
            model: rows

            delegate: RowLayout {
                id: row

                required property int index
                required property string destinationId
                required property string name
                required property string address
                required property real thresholdUs
                required property int sensitivity
                required property bool destinationEnabled

                readonly property bool addressValid: Utils.isValidAddress(address)

                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.preferredWidth: page.reorderColumnWidth
                    spacing: 0

                    QQC2.ToolButton {
                        icon.name: "arrow-up"
                        display: QQC2.AbstractButton.IconOnly
                        enabled: row.index > 0
                        onClicked: page.moveRow(row.index, row.index - 1)
                        QQC2.ToolTip.text: i18nc("@info:tooltip", "Move up")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                    QQC2.ToolButton {
                        icon.name: "arrow-down"
                        display: QQC2.AbstractButton.IconOnly
                        enabled: row.index < rows.count - 1
                        onClicked: page.moveRow(row.index, row.index + 1)
                        QQC2.ToolTip.text: i18nc("@info:tooltip", "Move down")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }

                QQC2.CheckBox {
                    Layout.preferredWidth: page.toggleColumnWidth
                    checked: row.destinationEnabled
                    onToggled: {
                        rows.setProperty(row.index, "destinationEnabled", checked);
                        page.save();
                    }
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Monitor this destination")
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.TextField {
                    text: row.name
                    placeholderText: i18nc("@info:placeholder", "Name")
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    onTextEdited: {
                        rows.setProperty(row.index, "name", text);
                        page.save();
                    }
                }

                QQC2.TextField {
                    id: addressField
                    text: row.address
                    placeholderText: i18nc("@info:placeholder", "IPv4, IPv6 or host name")
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    color: row.addressValid || text.length === 0
                           ? Kirigami.Theme.textColor
                           : Kirigami.Theme.negativeTextColor
                    onTextEdited: {
                        rows.setProperty(row.index, "address", text);
                        page.save();
                    }

                    Kirigami.Icon {
                        source: "dialog-warning"
                        visible: !row.addressValid
                        height: Kirigami.Units.iconSizes.small
                        width: height
                        anchors.right: parent.right
                        anchors.rightMargin: Kirigami.Units.smallSpacing
                        anchors.verticalCenter: parent.verticalCenter

                        QQC2.ToolTip.text: addressField.text.length === 0
                                           ? i18nc("@info:tooltip", "Enter an address")
                                           : i18nc("@info:tooltip", "This does not look like a valid address or host name")
                        QQC2.ToolTip.visible: warningHover.hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

                        HoverHandler {
                            id: warningHover
                        }
                    }
                }

                QQC2.SpinBox {
                    from: 1
                    to: 60000000
                    stepSize: 500
                    editable: true
                    value: row.thresholdUs
                    Layout.preferredWidth: page.thresholdColumnWidth
                    onValueModified: {
                        rows.setProperty(row.index, "thresholdUs", value);
                        page.save();
                    }
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Replies slower than this count as bad (%1)",
                                             Utils.formatThreshold(value))
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.SpinBox {
                    from: 0
                    to: 1000
                    editable: true
                    value: row.sensitivity
                    Layout.preferredWidth: page.sensitivityColumnWidth
                    onValueModified: {
                        rows.setProperty(row.index, "sensitivity", value);
                        page.save();
                    }
                    QQC2.ToolTip.text: i18np("One bad reply in a row flips the indicator",
                                             "%1 bad replies in a row flip the indicator", value + 1)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.ToolButton {
                    Layout.preferredWidth: page.removeColumnWidth
                    icon.name: "list-remove"
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: {
                        rows.remove(row.index);
                        page.save();
                    }
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Remove this destination")
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: true
            type: Kirigami.MessageType.Information
            text: i18n("<b>Threshold</b> is the round-trip time above which a reply is treated as bad. 1000 µs is 1 ms, which suits a wired gateway; hosts on the internet usually need 20000 µs or more.<br/><b>Sensitivity</b> is how many consecutive bad replies are tolerated before the indicator turns red. With the default of 3, one lost packet changes nothing but four in a row do; the same count applies when turning green again.")
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
