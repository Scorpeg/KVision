import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0

// One place to choose the picture each camera's tiles use: Auto, Main, Sub or Third.
// The choice is written onto that camera in every layout.
Window {
    id: win

    width: 860
    height: 640
    minimumWidth: 720
    minimumHeight: 320
    color: "#0f151b"
    title: qsTr("Video stream")
    flags: Qt.Window | Qt.WindowStaysOnTopHint

    property var rows: []
    readonly property int colW: 84
    readonly property int colGap: 16
    readonly property int colsRight: 14
    // Five columns, counted from the right edge: Exclude, Third, Sub, Main, Auto.
    function colX(rowWidth, fromRight) {
        return rowWidth - win.colsRight - win.colW - fromRight * (win.colW + win.colGap);
    }

    function byName(a, b) {
        var an = (a.name || "").toLowerCase();
        var bn = (b.name || "").toLowerCase();
        if (an < bn) return -1;
        if (an > bn) return 1;
        return 0;
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

    function reload() {
        var singles = [];
        var duals = [];
        var nvrs = [];
        var list = [];
        try { list = JSON.parse(rootWindow.hikvisionRecordersJson || "[]"); } catch (e) {}
        for (var i = 0; i < list.length; ++i) {
            var rec = list[i];
            if (rec.streamUrl)
                continue;
            var ip = (rec.ip || "").trim().split(":")[0];
            if (ip === "")
                continue;
            var cams = rec.cameras || [];
            var item = { name: rec.name || ip, ip: ip, cams: cams };
            if (cams.length > 2)
                nvrs.push(item);
            else if (cams.length === 2)
                duals.push(item);
            else
                singles.push(item);
        }
        singles.sort(win.byName);
        duals.sort(win.byName);
        nvrs.sort(win.byName);

        var out = [];
        for (var s = 0; s < singles.length; ++s) {
            var one = singles[s];
            var ch = win.channelOf(one.cams[0], 1);
            var oneKey = one.ip + "#" + ch;
            out.push({ kind: "camera", key: oneKey, group: one.ip, label: one.name });
            out = out.concat(win.layoutRows(oneKey));
        }
        for (var d = 0; d < duals.length; ++d) {
            var two = duals[d];
            out.push({ kind: "dual", label: two.name, group: two.ip });
            for (var lens = 0; lens < 2; ++lens) {
                var lc = win.channelOf(two.cams[lens], lens + 1);
                var lensKey = two.ip + "#" + lc;
                out.push({ kind: "lens", key: lensKey, group: two.ip,
                           label: win.camLabel(two.cams[lens], qsTr("Lens %1").arg(lens + 1)) });
                out = out.concat(win.layoutRows(lensKey));
            }
        }
        for (var n = 0; n < nvrs.length; ++n) {
            var nvr = nvrs[n];
            out.push({ kind: "nvr", label: nvr.name, group: nvr.ip });
            for (var j = 0; j < nvr.cams.length; ++j) {
                var nc = win.channelOf(nvr.cams[j], j + 1);
                var chanKey = nvr.ip + "#" + nc;
                out.push({ kind: "channel", key: chanKey, group: nvr.ip,
                           label: qsTr("Ch. %1  %2").arg(nc).arg(win.camLabel(nvr.cams[j], qsTr("Camera %1").arg(nc))) });
                out = out.concat(win.layoutRows(chanKey));
            }
        }
        win.rows = out;
    }

    function layoutRows(key) {
        var rows = [];
        for (var L = 0; L < layoutsCollectionModel.count; ++L) {
            var layout = layoutsCollectionModel.get(L);
            if (!layout || layout === rootWindow.scratchLayout || !layout.uid)
                continue;
            var found = false;
            for (var v = 0; v < layout.count; ++v) {
                var item = layout.get(v);
                if (item && rootWindow.tileKeyFromUrl(item.url) === key) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
            rows.push({ kind: "layout", key: key, layoutUid: layout.uid,
                        label: rootWindow.layoutDisplayName(layoutsCollectionModel, L) });
        }
        return rows;
    }

    function keysFor(group) {
        var keys = [];
        for (var i = 0; i < win.rows.length; ++i) {
            var r = win.rows[i];
            if (!r.key || r.kind === "layout" || (group !== "" && r.group !== group))
                continue;
            keys.push(r.key);
        }
        return keys;
    }

    function modeOf(key) {
        return rootWindow.cameraStreamMode(key);
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
            text: qsTr("Choose the picture each camera's tiles use. A choice takes effect at once on every layout that is not excluded. Auto uses the sub-stream on a multi-camera layout and the main stream when that tile is alone. Exclude leaves that layout as it is. A right-click on a tile sets that tile and ticks Exclude for its layout.")
            color: "#8898a6"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button { text: qsTr("All auto"); onClicked: rootWindow.setCameraStream(win.keysFor(""), 0) }
            Button { text: qsTr("All main"); onClicked: rootWindow.setCameraStream(win.keysFor(""), 1) }
            Button { text: qsTr("All sub"); onClicked: rootWindow.setCameraStream(win.keysFor(""), 2) }
            Button { text: qsTr("All third"); onClicked: rootWindow.setCameraStream(win.keysFor(""), 3) }
            Item { Layout.fillWidth: true }
        }

        Item {
            id: headings
            Layout.fillWidth: true
            Layout.preferredHeight: 22

            Text { x: win.colX(headings.width, 4); width: win.colW; text: qsTr("Auto"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 3); width: win.colW; text: qsTr("Main"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 2); width: win.colW; text: qsTr("Sub"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 1); width: win.colW; text: qsTr("Third"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
            Text { x: win.colX(headings.width, 0); width: win.colW; text: qsTr("Exclude"); color: "#00f5d4"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter }
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
                height: modelData.kind === "nvr" ? 34 : 32

                readonly property int nameInset: modelData.kind === "layout" ? 28 : (modelData.kind === "lens" || modelData.kind === "channel" ? 16 : 4)
                readonly property bool streamRow: modelData.kind === "camera" || modelData.kind === "lens" || modelData.kind === "channel"

                Rectangle {
                    anchors.fill: parent
                    color: (modelData.kind === "nvr" || modelData.kind === "dual") ? "#162028"
                           : (index % 2 === 0 ? "#1a2632" : "#10161c")
                }

                Text {
                    x: row.nameInset
                    width: Math.max(0, win.colX(row.width, 4) - row.nameInset - 8)
                    height: parent.height
                    text: modelData.label
                    color: modelData.kind === "nvr" ? "#00f5d4" : (modelData.kind === "layout" ? "#9aa8b2" : (streamRow ? "#eeeeee" : "#c5d0d8"))
                    font.pixelSize: modelData.kind === "layout" ? 12 : 13
                    font.bold: modelData.kind === "nvr" || modelData.kind === "dual"
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                RadioButton {
                    x: win.colX(row.width, 4) + (win.colW - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    visible: row.streamRow
                    padding: 0
                    autoExclusive: false
                    checked: win.modeOf(modelData.key) === 0
                    onClicked: {
                        rootWindow.setCameraStream([modelData.key], 0);
                        checked = Qt.binding(function() { return win.modeOf(modelData.key) === 0; });
                    }
                }
                RadioButton {
                    x: win.colX(row.width, 3) + (win.colW - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    visible: row.streamRow
                    padding: 0
                    autoExclusive: false
                    checked: win.modeOf(modelData.key) === 1
                    onClicked: {
                        rootWindow.setCameraStream([modelData.key], 1);
                        checked = Qt.binding(function() { return win.modeOf(modelData.key) === 1; });
                    }
                }
                RadioButton {
                    x: win.colX(row.width, 2) + (win.colW - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    visible: row.streamRow
                    padding: 0
                    autoExclusive: false
                    checked: win.modeOf(modelData.key) === 2
                    onClicked: {
                        rootWindow.setCameraStream([modelData.key], 2);
                        checked = Qt.binding(function() { return win.modeOf(modelData.key) === 2; });
                    }
                }
                RadioButton {
                    x: win.colX(row.width, 1) + (win.colW - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    visible: row.streamRow
                    padding: 0
                    autoExclusive: false
                    checked: win.modeOf(modelData.key) === 3
                    onClicked: {
                        rootWindow.setCameraStream([modelData.key], 3);
                        checked = Qt.binding(function() { return win.modeOf(modelData.key) === 3; });
                    }
                }
                CheckBox {
                    x: win.colX(row.width, 0) + (win.colW - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    visible: modelData.kind === "layout"
                    padding: 0
                    checked: rootWindow.layoutUidExcludes(modelData.layoutUid, modelData.key)
                    onToggled: {
                        rootWindow.setLayoutExcluded(modelData.layoutUid, modelData.key, checked);
                        checked = Qt.binding(function() { return rootWindow.layoutUidExcludes(modelData.layoutUid, modelData.key); });
                    }
                }
            }
        }
    }
}
