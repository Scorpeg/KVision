import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0
import CCTV_Viewer.Hikvision 1.0

// The result of "Max bitrate > Read all / Set on all cameras": one card per camera, added as each camera answers. The cards are the
// ones the detection setup check shows (SetupCheckList). kind is 1 main stream or 2 sub-stream; kbps 0 only reads, above 0 writes the
// max bitrate to every stream of that kind that accepts it (each camera is read back and checked). Opened by the Camera Tools box,
// which has asked first when it writes.
Window {
    id: win

    width: 780
    height: 520
    minimumWidth: 520
    minimumHeight: 300
    color: "#0f151b"
    property int kind: 2
    property int kbps: 0
    title: (kind === 1 ? qsTr("Main stream") : qsTr("Sub-stream")) + " - " + (kbps > 0 ? qsTr("set max bitrate to %1 kbps").arg(kbps) : qsTr("max bitrate"))
    flags: Qt.Window | Qt.WindowStaysOnTopHint

    property var results: []
    property string openKey: ""
    property bool running: false
    property string progressText: ""

    // Replaces the card of the same camera, or adds it at the end.
    function put(card) {
        var list = win.results.slice();
        for (var i = 0; i < list.length; ++i) {
            if (list[i].key === card.key) {
                list[i] = card;
                win.results = list;
                return;
            }
        }
        list.push(card);
        win.results = list;
    }

    function count(bad) {
        var n = 0;
        for (var i = 0; i < win.results.length; ++i) {
            var isBad = win.results[i].problems > 0 || win.results[i].warnings > 0;
            if (isBad === bad)
                ++n;
        }
        return n;
    }

    // Asks every camera, one after the other. The cards appear as the answers arrive. The button in this window only reads, so a write
    // is never repeated by accident: setting it again means pressing "Set on all cameras" in Camera Tools again, which asks first.
    function run(readOnly) {
        if (HikvisionBackup.ruleBusy)
            return;
        win.results = [];
        win.openKey = "";
        win.running = true;
        win.progressText = "";
        HikvisionBackup.streamBitrateAll(win.kind, readOnly === true ? 0 : win.kbps);
    }

    onClosing: {
        win.destroy();
        if (typeof rootWindow !== "undefined" && rootWindow)
            rootWindow.triggerGcDeferred();
    }

    Connections {
        target: HikvisionBackup
        function onStreamBitrateAllItem(card) { win.put(card); }
        function onStreamBitrateResult(card) { win.put(card); } // a card's own "Check this camera again"
        function onStreamBitrateAllProgress(name, done, total) {
            win.progressText = qsTr("Asking %1 of %2: %3").arg(done).arg(total).arg(name);
        }
        function onStreamBitrateAllFinished(summary, ok) {
            win.running = false;
            win.progressText = "";
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Text {
                text: win.running
                      ? (win.progressText !== "" ? win.progressText : qsTr("Asking the cameras..."))
                      : qsTr("%1 camera(s): %2 fine, %3 with something to note").arg(win.results.length).arg(win.count(false)).arg(win.count(true))
                color: "#8899aa"
                font.pixelSize: 12
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Button {
                text: qsTr("Read again")
                enabled: !win.running && !HikvisionBackup.ruleBusy
                onClicked: win.run(true)
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Read the max bitrate of every camera again (this only reads; it never writes again).")
            }
        }

        SetupCheckList {
            Layout.fillWidth: true
            Layout.fillHeight: true
            results: win.results
            busy: win.running || HikvisionBackup.ruleBusy
            onRecheck: HikvisionBackup.streamBitrate(key, win.kind, 0) // a card's own button only reads
            openKey: win.openKey
            onOpenKeyChanged: win.openKey = openKey
        }
    }
}
