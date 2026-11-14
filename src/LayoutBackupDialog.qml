import QtQuick 2.11
import QtQuick.Controls 2.4
import QtQuick.Layouts 1.11
import CCTV_Viewer.Hikvision 1.0

// Backups of one layout (opened from the Backup / Restore button), or the list of layouts that were deleted but still have
// backups (opened from "Recover a deleted layout"). A restore replaces only that one layout. Before it does, it saves the
// layout as it is now, so a restore can itself be undone.
Dialog {
    id: dlg
    modal: true
    anchors.centerIn: parent
    width: 600
    height: 440
    leftPadding: 14
    rightPadding: 14
    topPadding: 12
    bottomPadding: 12

    property int layoutIndex: -1      // the layout in the layouts list, or -1 in the recover mode
    property string uid: ""
    property string layoutTitle: ""
    property bool recoverMode: false
    property var entries: []
    property string status: ""
    property bool statusOk: true
    property string pendingId: ""     // the copy waiting for "Replace it"

    function liveUids() {
        var out = [];
        for (var i = 0; i < layoutsCollectionModel.count; ++i) {
            var l = layoutsCollectionModel.get(i);
            if (l && l.uid)
                out.push(l.uid);
        }
        return out;
    }

    function layoutLabel(l, i) {
        if (l && l.name && l.name.trim() !== "")
            return l.name;
        if (l && l.isNvr)
            return qsTr("Recorder %1").arg(l.nvrIp);
        return qsTr("Layout %1").arg(i + 1);
    }

    function refresh() {
        entries = recoverMode ? LayoutBackups.deletedLayouts(liveUids()) : LayoutBackups.list(uid);
    }

    function openFor(index) {
        var l = layoutsCollectionModel.get(index);
        if (!l)
            return;
        recoverMode = false;
        layoutIndex = index;
        uid = l.uid;
        layoutTitle = layoutLabel(l, index);
        noteField.text = "";
        status = "";
        pendingId = "";
        refresh();
        open();
    }

    function openRecover() {
        recoverMode = true;
        layoutIndex = -1;
        uid = "";
        layoutTitle = qsTr("Deleted layouts");
        status = "";
        pendingId = "";
        refresh();
        open();
    }

    function layoutJson(index) {
        return JSON.stringify(layoutsCollectionModel.toJSValue()[index]);
    }

    function hasCameras(json) {
        try {
            var items = JSON.parse(json).items || [];
            for (var i = 0; i < items.length; ++i)
                if (items[i].url)
                    return true;
        } catch (e) {}
        return false;
    }

    function backupNow() {
        var id = LayoutBackups.save(uid, layoutJson(layoutIndex), noteField.text.trim(), false);
        statusOk = id !== "";
        status = statusOk ? qsTr("Saved a copy of this layout.") : qsTr("Could not save the copy. Nothing was changed.");
        if (statusOk)
            noteField.text = "";
        refresh();
    }

    function restore(id) {
        var parsed = null;
        try { parsed = JSON.parse(LayoutBackups.load(id)); } catch (e) {}
        if (!parsed || !parsed.items) {
            statusOk = false;
            status = qsTr("That copy could not be read. Nothing was changed.");
            return;
        }
        if (recoverMode) {
            var added = layoutsCollectionModel.append();
            added.fromJSValue(parsed);
            statusOk = true;
            status = qsTr("The layout was put back at the end of the list.");
        } else {
            var now = layoutJson(layoutIndex);
            if (hasCameras(now))
                LayoutBackups.save(uid, now, qsTr("Before restore"), false);
            parsed.uid = uid; // it stays the same layout, so its copies stay together
            layoutsCollectionModel.get(layoutIndex).fromJSValue(parsed);
            statusOk = true;
            status = qsTr("Restored. The layout as it was before is saved too (\"Before restore\"), so this can be undone.");
        }
        pendingId = "";
        refresh();
    }

    function describeEntry(e) {
        var s = e.columns + "x" + e.rows + ", " + e.cameras + (e.cameras === 1 ? " camera" : " cameras");
        if (e.deleted)
            s += ", " + qsTr("copy taken when it was deleted");
        else if (e.note !== "")
            s += ", " + e.note;
        return s;
    }

    background: Rectangle {
        color: "#1c242c"
        border.color: "#2a3540"
        border.width: 1
        radius: 8
    }

    header: Rectangle {
        color: "#0f151b"
        height: 42
        implicitHeight: 42
        radius: 8
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 4; color: "#0f151b" }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#2a3540" }
        Text {
            anchors.fill: parent
            anchors.leftMargin: 16
            text: dlg.recoverMode ? qsTr("Recover a deleted layout") : qsTr("Backup / Restore: %1").arg(dlg.layoutTitle)
            color: "#00f5d4"
            font.bold: true
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    contentItem: ColumnLayout {
        spacing: 8

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: "#c8d2da"
            font.pixelSize: 11
            text: dlg.recoverMode
                  ? qsTr("These layouts were deleted but still have backups. Recover puts one back at the end of the list.")
                  : qsTr("Each copy holds this layout only: its grid size, cameras and positions. Restore replaces this layout with the copy and leaves every other layout, the recorders and the settings alone. KVision also saves a copy by itself about once a day when a layout has changed.")
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !dlg.recoverMode
            spacing: 8
            TextField {
                id: noteField
                Layout.fillWidth: true
                placeholderText: qsTr("A note for this copy (optional)")
                selectByMouse: true
                maximumLength: 120
                color: "white"
                background: Rectangle { color: "#0f151b"; radius: 4; border.color: noteField.activeFocus ? "#ff7a00" : "#2a3540" }
                onAccepted: dlg.backupNow()
            }
            Button {
                id: nowBtn
                text: qsTr("Back up now")
                implicitHeight: 30
                implicitWidth: 110
                contentItem: Text { text: nowBtn.text; color: "white"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: nowBtn.hovered ? "#059669" : "#10b981"; radius: 4 }
                onClicked: dlg.backupNow()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: dlg.status !== ""
            text: dlg.status
            color: dlg.statusOk ? "#2ecc71" : "#ff5a36"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0f151b"
            border.color: "#2a3540"
            radius: 4

            Text {
                anchors.centerIn: parent
                visible: dlg.entries.length === 0
                text: dlg.recoverMode ? qsTr("No deleted layouts have backups.") : qsTr("No copies of this layout yet. Press \"Back up now\".")
                color: "#8898a6"
                font.pixelSize: 12
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                spacing: 4
                model: dlg.entries
                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    width: list.width - 10
                    height: 46
                    color: "#141a21"
                    radius: 4
                    border.color: dlg.pendingId === modelData.id ? "#ff7a00" : "#2a3540"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 6
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: modelData.when + (dlg.recoverMode ? "   " + (modelData.name !== "" ? modelData.name : (modelData.nvrIp !== "" ? qsTr("Recorder %1").arg(modelData.nvrIp) : qsTr("(no name)"))) : "")
                                color: "#eeeeee"
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: dlg.describeEntry(modelData)
                                color: "#8898a6"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        Button {
                            id: restoreBtn
                            visible: dlg.pendingId !== modelData.id
                            text: dlg.recoverMode ? qsTr("Recover") : qsTr("Restore")
                            implicitHeight: 28
                            implicitWidth: 78
                            contentItem: Text { text: restoreBtn.text; color: "white"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: restoreBtn.hovered ? "#3a4550" : "#222c36"; radius: 4; border.color: "#00f5d4" }
                            onClicked: {
                                if (dlg.recoverMode)
                                    dlg.restore(modelData.id);
                                else
                                    dlg.pendingId = modelData.id;
                            }
                        }
                        Button {
                            id: sureBtn
                            visible: dlg.pendingId === modelData.id
                            text: qsTr("Replace it")
                            implicitHeight: 28
                            implicitWidth: 86
                            contentItem: Text { text: sureBtn.text; color: "white"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: sureBtn.hovered ? "#ff4d4d" : "#d63333"; radius: 4 }
                            onClicked: dlg.restore(modelData.id)
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("Replaces this layout with the copy from %1. The layout as it is now is saved first.").arg(modelData.when)
                        }
                        Button {
                            id: cancelPendingBtn
                            visible: dlg.pendingId === modelData.id
                            text: qsTr("Cancel")
                            implicitHeight: 28
                            implicitWidth: 64
                            contentItem: Text { text: cancelPendingBtn.text; color: "white"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: cancelPendingBtn.hovered ? "#3a4550" : "#222c36"; radius: 4 }
                            onClicked: dlg.pendingId = ""
                        }
                        Button {
                            id: removeBtn
                            visible: dlg.pendingId !== modelData.id
                            implicitHeight: 28
                            implicitWidth: 28
                            contentItem: Text { text: "\u00d7"; color: removeBtn.hovered ? "#ff4d4d" : "#8898a6"; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: "transparent"; radius: 14; border.color: removeBtn.hovered ? "#ff4d4d" : "#2a3540" }
                            onClicked: { LayoutBackups.remove(modelData.id); dlg.refresh(); }
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("Delete this copy")
                        }
                    }
                }
            }
        }
    }

    footer: Rectangle {
        implicitHeight: 48
        color: "#0f151b"
        border.color: "#2a3540"
        radius: 8
        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            Text {
                Layout.fillWidth: true
                text: qsTr("Kept in %1 (the newest 30 copies of each layout).").arg(LayoutBackups.folder())
                color: "#8898a6"
                font.pixelSize: 10
                elide: Text.ElideMiddle
            }
            Button {
                id: closeBtn
                text: qsTr("Close")
                implicitWidth: 80
                implicitHeight: 30
                contentItem: Text { text: closeBtn.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: closeBtn.hovered ? "#3a4550" : "#222c36"; radius: 4; border.color: "#2a3540" }
                onClicked: dlg.close()
            }
        }
    }
}
