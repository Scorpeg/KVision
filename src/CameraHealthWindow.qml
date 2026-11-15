import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0
import CCTV_Viewer.Hikvision 1.0

// The result of "Camera clock and firmware > Check all": one card per camera, added as each camera answers. The cards are the
// ones the detection setup check shows (SetupCheckList), so both look and behave the same. It only reads: nothing on a camera is
// changed.
Window {
    id: win

    width: 780
    height: 520
    minimumWidth: 520
    minimumHeight: 300
    color: "#0f151b"
    title: qsTr("Camera clock and firmware")
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

    // Asks every camera, one after the other (read only). The cards appear as the answers arrive.
    function run() {
        if (HikvisionBackup.ruleBusy)
            return;
        win.results = [];
        win.openKey = "";
        win.running = true;
        win.progressText = "";
        HikvisionBackup.checkHealthAll();
    }

    onClosing: {
        win.destroy();
        if (typeof rootWindow !== "undefined" && rootWindow)
            rootWindow.triggerGcDeferred();
    }

    Connections {
        target: HikvisionBackup
        function onHealthAllItem(card) { win.put(card); }
        function onHealthResult(card) { win.put(card); } // a card's own "Check this camera again"
        function onHealthAllProgress(name, done, total) {
            win.progressText = qsTr("Asking %1 of %2: %3").arg(done).arg(total).arg(name);
        }
        function onHealthAllFinished(summary, ok) {
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
                      : qsTr("%1 camera(s): %2 fine, %3 to look at").arg(win.results.length).arg(win.count(false)).arg(win.count(true))
                color: "#8899aa"
                font.pixelSize: 12
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Button {
                text: qsTr("Check again")
                enabled: !win.running && !HikvisionBackup.ruleBusy
                onClicked: win.run()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Ask every camera again (read only).")
            }
        }

        SetupCheckList {
            Layout.fillWidth: true
            Layout.fillHeight: true
            results: win.results
            busy: win.running || HikvisionBackup.ruleBusy
            onRecheck: HikvisionBackup.checkHealth(key)
            openKey: win.openKey
            onOpenKeyChanged: win.openKey = openKey
        }
    }
}
