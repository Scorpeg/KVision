import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0
import CCTV_Viewer.Utils 1.0

// One place to choose the stream saved on each layout. A layout opens to its cameras.
// A choice is written only onto that layout.
Window {
    id: win

    width: 760
    height: 640
    minimumWidth: 640
    minimumHeight: 320
    color: "#0f151b"
    title: qsTr("Video Stream per Layout")
    flags: Qt.Window | Qt.WindowStaysOnTopHint

    property var rows: []
    property var openUids: ({})
    readonly property int colW: 84
    readonly property int colGap: 16
    readonly property int colsRight: 14
    // Four columns, counted from the right edge: Third, Sub, Main, Auto.
    function colX(rowWidth, fromRight) {
        return rowWidth - win.colsRight - win.colW - fromRight * (win.colW + win.colGap);
    }

    function channelOf(cam, fallback) {
        var c = parseInt(cam && cam.channelId);
        return isNaN(c) ? fallback : c;
    }

    function camLabel(cam, fallback) {
        if (!cam)
            return fallback;
        var name = (cam.customName || cam.name || "").trim();
        return name !== "" ? name : fallback;
    }

    // "Camera 01" is the name a recorder ships with. It does not say which recorder it is.
    function isGenericName(name) {
        return /^camera( \d+)?$/i.test((name || "").trim()) || /^ip\s*camera( \d+)?$/i.test((name || "").trim());
    }

    function cameraNames() {
        var names = {};
        var list = [];
        try { list = JSON.parse(rootWindow.hikvisionRecordersJson || "[]"); } catch (e) {}
        for (var i = 0; i < list.length; ++i) {
            var rec = list[i];
            if (rec.streamUrl)
                continue;
            var ip = (rec.ip || "").trim().split(":")[0];
            if (ip === "")
                continue;
            var recName = (rec.name || ip).trim();
            var cams = rec.cameras || [];
            var own = Utils.isStandaloneRecorder(rec);
            if (cams.length === 0) {
                names[ip + "#1"] = { cam: "", rec: recName, standalone: own, channel: 1, ip: ip };
                continue;
            }
            for (var j = 0; j < cams.length; ++j) {
                var ch = win.channelOf(cams[j], j + 1);
                names[ip + "#" + ch] = { cam: win.camLabel(cams[j], ""), rec: recName, standalone: own, channel: ch, ip: ip };
            }
        }
        return names;
    }

    // A factory name, or the same name on two cameras, is shown with the recorder.
    // Two factory names on one recorder stay distinct (Camera 01 and Camera 02).
    function displayName(entry, sameName, key) {
        var cam = (entry.cam || "").trim();
        var rec = (entry.rec || "").trim();
        if (cam === "" || win.isGenericName(cam)) {
            var label = (rec && cam) ? (rec + " — " + cam) : (rec || cam);
            if (sameName > 1 && key) {
                var ch = String(key).split("#")[1];
                if (ch)
                    label = label + " ch " + ch;
            }
            return label;
        }
        if (sameName > 1) {
            if (rec && rec.toLowerCase().indexOf(cam.toLowerCase()) >= 0)
                return rec;
            return rec ? (rec + " — " + cam) : cam;
        }
        return cam;
    }

    function keysOn(layout) {
        var keys = [];
        var seen = {};
        if (!layout)
            return keys;
        for (var v = 0; v < layout.count; ++v) {
            var item = layout.get(v);
            var key = item ? rootWindow.tileKeyFromUrl(item.url) : "";
            if (key === "" || seen[key])
                continue;
            seen[key] = true;
            keys.push(key);
        }
        return keys;
    }

    function cameraMode(uid, key) {
        var rev = rootWindow.tileStreamRev;
        var layout = rootWindow.layoutByUid(uid);
        if (!layout || !key)
            return 0;
        var seen = -2;
        for (var v = 0; v < layout.count; ++v) {
            var item = layout.get(v);
            if (!item || rootWindow.tileKeyFromUrl(item.url) !== key)
                continue;
            var mode = item.streamMode;
            if (mode !== 1 && mode !== 2 && mode !== 3)
                mode = 0;
            if (seen === -2)
                seen = mode;
            else if (seen !== mode)
                return -1;
        }
        return seen === -2 ? 0 : seen;
    }

    function layoutMode(uid) {
        var rev = rootWindow.tileStreamRev;
        var layout = rootWindow.layoutByUid(uid);
        var keys = win.keysOn(layout);
        if (keys.length === 0)
            return -1;
        var mode = win.cameraMode(uid, keys[0]);
        for (var i = 1; i < keys.length; ++i) {
            if (win.cameraMode(uid, keys[i]) !== mode)
                return -1;
        }
        return mode;
    }

    function choose(uid, key, mode) {
        rootWindow.setLayoutCameraStream(rootWindow.layoutByUid(uid), key, mode);
    }

    function chooseLayout(uid, mode) {
        var layout = rootWindow.layoutByUid(uid);
        var keys = win.keysOn(layout);
        for (var i = 0; i < keys.length; ++i)
            rootWindow.setLayoutCameraStream(layout, keys[i], mode);
    }

    function toggle(uid) {
        var open = Object.assign({}, win.openUids);
        open[uid] = !open[uid];
        win.openUids = open;
        var y = view.contentY;
        win.reload();
        Qt.callLater(function() { view.contentY = y; });
    }

    function reload() {
        var names = win.cameraNames();
        var out = [];
        for (var L = 0; L < layoutsCollectionModel.count; ++L) {
            var layout = layoutsCollectionModel.get(L);
            if (!layout || layout === rootWindow.scratchLayout || !layout.uid)
                continue;
            var keys = win.keysOn(layout);
            var counts = {};
            for (var k = 0; k < keys.length; ++k) {
                var entry = names[keys[k]] || { cam: keys[k], rec: "" };
                var raw = (entry.cam || "").trim() || keys[k];
                counts[raw.toLowerCase()] = (counts[raw.toLowerCase()] || 0) + 1;
            }
            var labeled = [];
            for (k = 0; k < keys.length; ++k) {
                entry = names[keys[k]] || { cam: keys[k], rec: "", standalone: true, channel: 0, ip: "" };
                raw = (entry.cam || "").trim() || keys[k];
                var parts = String(keys[k]).split("#");
                var chNum = parseInt(parts[1]);
                labeled.push({
                    key: keys[k],
                    name: win.displayName(entry, counts[raw.toLowerCase()] || 1, keys[k]),
                    standalone: entry.standalone !== false,
                    channel: isNaN(chNum) ? (entry.channel || 0) : chNum,
                    ip: entry.ip || parts[0] || ""
                });
            }
            labeled.sort(Utils.compareCameraChoice);
            out.push({
                kind: "layout",
                layoutUid: layout.uid,
                label: rootWindow.layoutDisplayName(layoutsCollectionModel, L),
                open: !!win.openUids[layout.uid],
                current: layout === layoutsCollectionModel.get(rootWindow.layoutIndex),
                hasCameras: keys.length > 0
            });
            if (!win.openUids[layout.uid])
                continue;
            if (labeled.length === 0) {
                out.push({ kind: "empty", layoutUid: layout.uid, label: qsTr("No cameras on this layout") });
                continue;
            }
            for (var c = 0; c < labeled.length; ++c) {
                out.push({
                    kind: "camera",
                    layoutUid: layout.uid,
                    key: labeled[c].key,
                    label: labeled[c].name
                });
            }
        }
        win.rows = out;
    }

    onVisibleChanged: {
        if (visible)
            win.reload();
    }

    onClosing: {
        win.destroy();
        if (typeof rootWindow !== "undefined" && rootWindow)
            rootWindow.triggerGcDeferred();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: qsTr("Each layout keeps its own stream for each camera. Open a layout to set its cameras. Auto uses the sub-stream on a multi-camera layout and the main stream when that tile is alone. A choice on a layout row sets every camera in that layout.")
            color: "#8898a6"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        Item {
            id: headings
            Layout.fillWidth: true
            Layout.preferredHeight: 22

            Text { x: win.colX(headings.width, 3); width: win.colW; text: qsTr("Auto"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 2); width: win.colW; text: qsTr("Main"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 1); width: win.colW; text: qsTr("Sub"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 0); width: win.colW; text: qsTr("Third"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
        }

        ListView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: win.rows
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                width: view.width
                height: modelData.kind === "layout" ? 36 : 32
                readonly property var rowData: modelData
                readonly property bool choiceRow: modelData.kind === "camera" || (modelData.kind === "layout" && modelData.hasCameras)

                Rectangle {
                    anchors.fill: parent
                    color: modelData.kind === "layout" ? "#162028"
                           : (index % 2 === 0 ? "#1a2632" : "#10161c")
                }

                MouseArea {
                    x: 0
                    width: Math.max(0, win.colX(row.width, 3) - 4)
                    height: parent.height
                    enabled: modelData.kind === "layout"
                    onClicked: win.toggle(modelData.layoutUid)
                }

                Text {
                    x: modelData.kind === "camera" ? 28 : 8
                    width: Math.max(0, win.colX(row.width, 3) - x - 8)
                    height: parent.height
                    text: (modelData.kind === "layout" ? (modelData.open ? "▾  " : "▸  ") : "") + modelData.label
                    color: modelData.kind === "empty" ? "#6d7b86"
                           : (modelData.kind === "layout" ? (modelData.current ? "#00f5d4" : "#d5dee6") : "#eeeeee")
                    font.pixelSize: modelData.kind === "layout" ? 14 : 13
                    font.bold: modelData.kind === "layout"
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Repeater {
                    model: row.choiceRow ? [0, 1, 2, 3] : []
                    RadioButton {
                        readonly property int mode: modelData
                        readonly property int fromRight: 3 - modelData
                        x: win.colX(row.width, fromRight) + (win.colW - width) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 22
                        padding: 0
                        autoExclusive: false
                        checked: row.rowData.kind === "layout"
                                 ? win.layoutMode(row.rowData.layoutUid) === mode
                                 : win.cameraMode(row.rowData.layoutUid, row.rowData.key) === mode
                        onClicked: {
                            if (row.rowData.kind === "layout")
                                win.chooseLayout(row.rowData.layoutUid, mode);
                            else
                                win.choose(row.rowData.layoutUid, row.rowData.key, mode);
                            checked = Qt.binding(function() {
                                return row.rowData.kind === "layout"
                                        ? win.layoutMode(row.rowData.layoutUid) === mode
                                        : win.cameraMode(row.rowData.layoutUid, row.rowData.key) === mode;
                            });
                        }
                    }
                }
            }
        }
    }
}
