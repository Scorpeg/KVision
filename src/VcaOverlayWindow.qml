import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0
import CCTV_Viewer.Utils 1.0

// One place to switch on-screen target boxes and VCA lines for every camera.
// It writes the same lists the tile's right-click items write, so the two always agree.
Window {
    id: win

    width: 760
    height: 560
    minimumWidth: 620
    minimumHeight: 260
    color: "#0f151b"
    title: qsTr("Target boxes and VCA lines")
    flags: Qt.Window | Qt.WindowStaysOnTopHint

    property var rows: []
    readonly property int colW: 150
    readonly property int colGap: 36

    function parseList(text) {
        try {
            var l = JSON.parse(text || "[]");
            return Array.isArray(l) ? l : [];
        } catch (e) {
            return [];
        }
    }

    function listed(text, key) {
        return win.parseList(text).indexOf(key) >= 0;
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
                continue; // a saved RTSP or HTTP address has no VCA data
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
        var own = singles.concat(duals);
        own.sort(win.byName);
        nvrs.sort(win.byName);

        var out = [];
        for (var s = 0; s < own.length; ++s) {
            var dev = own[s];
            if ((dev.cams || []).length === 2) {
                out.push({ kind: "dual", label: dev.name, group: dev.ip });
                var lenses = Utils.sortByChannel(dev.cams);
                for (var lens = 0; lens < lenses.length; ++lens) {
                    var lc = win.channelOf(lenses[lens], lens + 1);
                    out.push({ kind: "lens", key: dev.ip + "#" + lc, group: dev.ip,
                               label: win.camLabel(lenses[lens], qsTr("Lens %1").arg(lens + 1)) });
                }
            } else {
                var ch = win.channelOf(dev.cams[0], 1);
                out.push({ kind: "camera", key: dev.ip + "#" + ch, group: dev.ip, label: dev.name });
            }
        }
        for (var n = 0; n < nvrs.length; ++n) {
            var nvr = nvrs[n];
            out.push({ kind: "nvr", label: nvr.name, group: nvr.ip });
            var channels = Utils.sortByChannel(nvr.cams);
            for (var j = 0; j < channels.length; ++j) {
                var nc = win.channelOf(channels[j], j + 1);
                out.push({ kind: "channel", key: nvr.ip + "#" + nc, group: nvr.ip,
                           label: qsTr("Ch. %1  %2").arg(nc).arg(win.camLabel(channels[j], qsTr("Camera %1").arg(nc))) });
            }
        }
        win.rows = out;
    }

    // which is "boxes" or "lines". group "" means every camera.
    function setMany(group, which, on) {
        var text = which === "lines" ? generalSettings.vcaRuleLinesOnCameras : generalSettings.vcaTargetsOnCameras;
        var l = win.parseList(text);
        var changed = false;
        for (var i = 0; i < win.rows.length; ++i) {
            var r = win.rows[i];
            if (!r.key || (group !== "" && r.group !== group))
                continue;
            var at = l.indexOf(r.key);
            if (on && at < 0) { l.push(r.key); changed = true; }
            else if (!on && at >= 0) { l.splice(at, 1); changed = true; }
        }
        if (!changed)
            return;
        var saved = JSON.stringify(l);
        if (which === "lines")
            generalSettings.vcaRuleLinesOnCameras = saved;
        else
            generalSettings.vcaTargetsOnCameras = saved;
    }

    function setOne(key, which, on) {
        var text = which === "lines" ? generalSettings.vcaRuleLinesOnCameras : generalSettings.vcaTargetsOnCameras;
        var l = win.parseList(text);
        var at = l.indexOf(key);
        if (on && at < 0)
            l.push(key);
        else if (!on && at >= 0)
            l.splice(at, 1);
        else
            return;
        var saved = JSON.stringify(l);
        if (which === "lines")
            generalSettings.vcaRuleLinesOnCameras = saved;
        else
            generalSettings.vcaTargetsOnCameras = saved;
    }

    onVisibleChanged: {
        if (visible)
            win.reload();
    }

    onClosing: {
        win.destroy();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: qsTr("Tick a camera to draw on its tiles. A tick takes effect at once and is remembered. The camera itself is not changed. The right-click menu on a tile shows the same ticks.")
            color: "#8898a6"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button { text: qsTr("All boxes on"); onClicked: win.setMany("", "boxes", true) }
            Button { text: qsTr("All boxes off"); onClicked: win.setMany("", "boxes", false) }
            Button { text: qsTr("All lines on"); onClicked: win.setMany("", "lines", true) }
            Button { text: qsTr("All lines off"); onClicked: win.setMany("", "lines", false) }
            Item { Layout.fillWidth: true }
        }

        // The two headings sit in the same columns as the ticks, with a gap between them.
        Row {
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 14
            spacing: 0
            Item { width: parent.width - win.colW * 2 - win.colGap; height: 1 }
            Text {
                width: win.colW
                text: qsTr("Target Boxes")
                color: "#00f5d4"
                font.pixelSize: 12
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            Item { width: win.colGap; height: 1 }
            Text {
                width: win.colW
                text: qsTr("VCA Lines")
                color: "#00f5d4"
                font.pixelSize: 12
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
        }

        ListView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: win.rows
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                width: view.width
                height: modelData.kind === "nvr" ? 36 : (modelData.kind === "dual" ? 26 : 30)

                Rectangle {
                    anchors.fill: parent
                    color: (modelData.kind === "nvr" || modelData.kind === "dual") ? "#162028"
                           : (index % 2 === 0 ? "#1a2632" : "#10161c")
                }

                Row {
                    anchors.fill: parent
                    anchors.rightMargin: 14
                    anchors.leftMargin: modelData.kind === "lens" || modelData.kind === "channel" ? 18 : 4
                    spacing: 0
                    visible: modelData.kind === "dual" || modelData.kind === "nvr"

                    Text {
                        width: parent.width - win.colW * 2 - win.colGap
                        height: parent.height
                        text: modelData.label
                        color: modelData.kind === "nvr" ? "#00f5d4" : "#c5d0d8"
                        font.pixelSize: modelData.kind === "nvr" ? 13 : 12
                        font.bold: true
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    Item { width: win.colW; height: 1 }
                    Item { width: win.colGap; height: 1 }
                    Item { width: win.colW; height: 1 }
                }

                Row {
                    anchors.fill: parent
                    anchors.rightMargin: 14
                    anchors.leftMargin: modelData.kind === "camera" ? 4 : 18
                    spacing: 0
                    visible: modelData.kind === "camera" || modelData.kind === "lens" || modelData.kind === "channel"

                    Text {
                        width: parent.width - win.colW * 2 - win.colGap
                        height: parent.height
                        text: modelData.label
                        color: "#eeeeee"
                        font.pixelSize: 13
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    Item {
                        width: win.colW
                        height: parent.height
                        CheckBox {
                            anchors.centerIn: parent
                            checked: win.listed(generalSettings.vcaTargetsOnCameras, modelData.key)
                            onToggled: win.setOne(modelData.key, "boxes", checked)
                        }
                    }
                    Item { width: win.colGap; height: 1 }
                    Item {
                        width: win.colW
                        height: parent.height
                        CheckBox {
                            anchors.centerIn: parent
                            checked: win.listed(generalSettings.vcaRuleLinesOnCameras, modelData.key)
                            onToggled: win.setOne(modelData.key, "lines", checked)
                        }
                    }
                }
            }
        }
    }
}
