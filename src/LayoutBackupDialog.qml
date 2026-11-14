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
    property bool pickMode: false     // "Use another layout": apply a different layout's shape to this one
    property bool copyCameras: false  // in the pick mode: bring the other layout's cameras too

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
        var out = [];
        var i, e, src;
        if (pickMode) {
            // Every other layout as it is now, then the deleted ones (their newest copy).
            var all = layoutsCollectionModel.toJSValue();
            for (i = 0; i < all.length; ++i) {
                if (i === layoutIndex || all[i].uid === uid)
                    continue;
                var l = all[i];
                var cams = 0;
                for (var k = 0; k < l.items.length; ++k)
                    if (l.items[k].url)
                        ++cams;
                out.push({ key: "live:" + i, id: "", removable: false, uid: l.uid, liveIndex: i,
                           title: layoutLabel(layoutsCollectionModel.get(i), i),
                           sub: l.size.width + "x" + l.size.height + ", " + cams + (cams === 1 ? " camera" : " cameras") + ", " + qsTr("as it is now") });
            }
            var gone = LayoutBackups.deletedLayouts(liveUids());
            for (i = 0; i < gone.length; ++i) {
                e = gone[i];
                out.push({ key: "bk:" + e.id, id: e.id, removable: false, uid: e.uid, liveIndex: -1,
                           title: (e.name !== "" ? e.name : (e.nvrIp !== "" ? qsTr("Recorder %1").arg(e.nvrIp) : qsTr("(no name)"))) + "   (" + qsTr("deleted") + ")",
                           sub: e.columns + "x" + e.rows + ", " + e.cameras + (e.cameras === 1 ? " camera" : " cameras") + ", " + qsTr("copy from %1").arg(e.when) });
            }
        } else {
            src = recoverMode ? LayoutBackups.deletedLayouts(liveUids()) : LayoutBackups.list(uid);
            for (i = 0; i < src.length; ++i) {
                e = src[i];
                out.push({ key: e.id, id: e.id, removable: true, uid: e.uid, liveIndex: -1,
                           title: e.when + (recoverMode ? "   " + (e.name !== "" ? e.name : (e.nvrIp !== "" ? qsTr("Recorder %1").arg(e.nvrIp) : qsTr("(no name)"))) : ""),
                           sub: describeEntry(e), when: e.when });
            }
        }
        entries = out;
    }

    function openFor(index) {
        var l = layoutsCollectionModel.get(index);
        if (!l)
            return;
        recoverMode = false;
        pickMode = false;
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
        pickMode = false;
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

    // The layout JSON a picked entry stands for, or null.
    function pickedLayout(entry) {
        if (entry.liveIndex >= 0) {
            var all = layoutsCollectionModel.toJSValue();
            return (entry.liveIndex < all.length && all[entry.liveIndex].uid === entry.uid) ? all[entry.liveIndex] : null;
        }
        try { return JSON.parse(LayoutBackups.load(entry.id)); } catch (e) { return null; }
    }

    // Puts another layout's grid size and merged cells on this layout, and (when asked) its cameras. This layout keeps its own
    // name, its place in the list, its toolbar setting and its backups. Without the cameras, this layout's own cameras stay
    // where they are when that spot still exists and is not part of a merged cell; the ones that do not fit are counted.
    function applyPicked(entry) {
        var src = pickedLayout(entry);
        if (!src || !src.items || !src.size) {
            statusOk = false;
            status = qsTr("That layout could not be read. Nothing was changed.");
            return;
        }
        var cur = layoutJson(layoutIndex);
        var curObj = JSON.parse(cur);
        if (hasCameras(cur))
            LayoutBackups.save(uid, cur, qsTr("Before applying another layout"), false);

        var W = src.size.width, H = src.size.height;
        var cw = curObj.size.width, ch = curObj.size.height;
        var items = [];
        var lost = 0;
        var placed = {};
        // The cells a merged cell covers (worked out from the spans, so it does not depend on the hidden flag being set).
        var covered = {};
        for (var r0 = 0; r0 < H; ++r0) {
            for (var c0 = 0; c0 < W; ++c0) {
                var o = src.items[r0 * W + c0];
                if (!o || o.visible === 1 || o.columnSpan < 1 || o.rowSpan < 1)
                    continue;
                for (var dr = 0; dr < o.rowSpan && r0 + dr < H; ++dr)
                    for (var dc = 0; dc < o.columnSpan && c0 + dc < W; ++dc)
                        if (dr !== 0 || dc !== 0)
                            covered[(r0 + dr) * W + (c0 + dc)] = true;
            }
        }
        for (var r = 0; r < H; ++r) {
            for (var c = 0; c < W; ++c) {
                var s = src.items[r * W + c] || {};
                var item;
                if (copyCameras) {
                    item = JSON.parse(JSON.stringify(s));
                } else {
                    var mine = (c < cw && r < ch) ? curObj.items[r * cw + c] : null;
                    item = mine ? JSON.parse(JSON.stringify(mine))
                                : { avFormatOptions: {}, ignoreGlobalAVFormatOptions: false, noUnmute: false, secondaryUrl: "", streamMode: 0, url: "", volume: 0 };
                    item.columnSpan = s.columnSpan !== undefined ? s.columnSpan : 1;
                    item.rowSpan = s.rowSpan !== undefined ? s.rowSpan : 1;
                    item.visible = s.visible !== undefined ? s.visible : 0;
                    if (covered[r * W + c] === true || item.visible !== 0) {      // a cell inside a merged one shows no camera of its own
                        item.url = "";
                        item.secondaryUrl = "";
                    }
                    if (mine && mine.url)
                        placed[(r * cw + c)] = item.url !== "";
                }
                items.push(item);
            }
        }
        if (!copyCameras) {
            for (var i = 0; i < curObj.items.length; ++i)
                if (curObj.items[i].url && placed[i] !== true)
                    ++lost;
        }
        layoutsCollectionModel.get(layoutIndex).fromJSValue({ size: { width: W, height: H },
                                                              aspectRatio: src.aspectRatio || curObj.aspectRatio, items: items });
        statusOk = true;
        status = qsTr("Done: this layout is now %1x%2.").arg(W).arg(H)
                 + (copyCameras ? " " + qsTr("Its cameras were copied too.") : (lost > 0 ? " " + qsTr("%1 of this layout's cameras did not fit and were removed from it; they are in the \"Before applying another layout\" copy.").arg(lost) : ""))
                 + " " + qsTr("The layout as it was is saved (\"Before applying another layout\"), so this can be undone.");
        pendingId = "";
        pickMode = false;
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
            text: dlg.recoverMode ? qsTr("Recover a deleted layout") : (dlg.pickMode ? qsTr("Use another layout for: %1").arg(dlg.layoutTitle) : qsTr("Backup / Restore: %1").arg(dlg.layoutTitle))
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
                  : dlg.pickMode
                  ? qsTr("Pick a layout to copy the grid of: its size and merged cells. This layout keeps its name, its place in the list and its toolbar setting.")
                  : qsTr("Each copy holds this layout only: its grid size, cameras and positions. Restore replaces this layout with the copy and leaves every other layout, the recorders and the settings alone. KVision also saves a copy by itself about once a day when a layout has changed.")
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !dlg.recoverMode && !dlg.pickMode
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

        RowLayout {
            Layout.fillWidth: true
            visible: !dlg.recoverMode
            spacing: 10
            Button {
                id: pickBtn
                text: dlg.pickMode ? qsTr("Back to the copies") : qsTr("Use another layout...")
                implicitHeight: 28
                implicitWidth: 170
                contentItem: Text { text: pickBtn.text; color: pickBtn.hovered ? "#00f5d4" : "#c8d2da"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: pickBtn.hovered ? "#1f2a34" : "transparent"; radius: 4; border.color: pickBtn.hovered ? "#00f5d4" : "#2a3540" }
                onClicked: { dlg.pickMode = !dlg.pickMode; dlg.pendingId = ""; dlg.status = ""; dlg.refresh(); }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Pick one of your other layouts (or a deleted one) and give this layout its grid.")
            }
            CheckBox {
                visible: dlg.pickMode
                text: qsTr("Bring its cameras too")
                checked: dlg.copyCameras
                onToggled: dlg.copyCameras = checked
                palette.highlight: "#00f5d4"
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Off (the default): only the grid changes, and this layout's own cameras stay where they still fit. On: the other layout's cameras come too, and this layout's cameras are replaced.")
            }
            Item { Layout.fillWidth: true }
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
                text: dlg.recoverMode ? qsTr("No deleted layouts have backups.") : (dlg.pickMode ? qsTr("There is no other layout to use.") : qsTr("No copies of this layout yet. Press \"Back up now\"."))
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
                    border.color: dlg.pendingId === modelData.key ? "#ff7a00" : "#2a3540"

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
                                text: modelData.title
                                color: "#eeeeee"
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.sub
                                color: "#8898a6"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        Button {
                            id: restoreBtn
                            visible: dlg.pendingId !== modelData.key
                            text: dlg.recoverMode ? qsTr("Recover") : (dlg.pickMode ? qsTr("Use this") : qsTr("Restore"))
                            implicitHeight: 28
                            implicitWidth: 78
                            contentItem: Text { text: restoreBtn.text; color: "white"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: restoreBtn.hovered ? "#3a4550" : "#222c36"; radius: 4; border.color: "#00f5d4" }
                            onClicked: {
                                if (dlg.recoverMode)
                                    dlg.restore(modelData.id);
                                else
                                    dlg.pendingId = modelData.key;
                            }
                        }
                        Button {
                            id: sureBtn
                            visible: dlg.pendingId === modelData.key
                            text: dlg.pickMode ? qsTr("Apply it") : qsTr("Replace it")
                            implicitHeight: 28
                            implicitWidth: 86
                            contentItem: Text { text: sureBtn.text; color: "white"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: sureBtn.hovered ? "#ff4d4d" : "#d63333"; radius: 4 }
                            onClicked: { if (dlg.pickMode) dlg.applyPicked(modelData); else dlg.restore(modelData.id); }
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: dlg.pickMode ? qsTr("Changes this layout to the grid of \"%1\". The layout as it is now is saved first, so this can be undone.").arg(modelData.title)
                                                       : qsTr("Replaces this layout with the copy from %1. The layout as it is now is saved first.").arg(modelData.when)
                        }
                        Button {
                            id: cancelPendingBtn
                            visible: dlg.pendingId === modelData.key
                            text: qsTr("Cancel")
                            implicitHeight: 28
                            implicitWidth: 64
                            contentItem: Text { text: cancelPendingBtn.text; color: "white"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { color: cancelPendingBtn.hovered ? "#3a4550" : "#222c36"; radius: 4 }
                            onClicked: dlg.pendingId = ""
                        }
                        Button {
                            id: removeBtn
                            visible: dlg.pendingId !== modelData.key && modelData.removable
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
