import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0
import CCTV_Viewer.Hikvision 1.0

// The result of the right-click "Quick tools > Camera status": one card for the camera that was clicked. It reads the camera's own
// status page and never changes anything on it. "Check this camera again" on the card asks again.
Window {
    id: win

    property string camIp: ""
    property string camName: ""
    property var results: []
    property bool waiting: true

    width: 560
    height: 440
    minimumWidth: 420
    minimumHeight: 240
    color: "#0f151b"
    title: qsTr("Camera status") + (camName !== "" ? " - " + camName : "")
    flags: Qt.Window | Qt.WindowStaysOnTopHint

    // Asks the camera. The camera list that the check needs is loaded first when it is not there yet.
    function run() {
        if (HikvisionBackup.ruleBusy)
            return;
        win.waiting = true;
        if (HikvisionBackup.cameraCount() === 0)
            HikvisionBackup.loadCameras(rootWindow.hikvisionRecordersJson || "[]", Context.config.fileName);
        else
            HikvisionBackup.checkStatus(win.camIp);
    }

    onClosing: {
        win.destroy();
        if (typeof rootWindow !== "undefined" && rootWindow)
            rootWindow.triggerGcDeferred();
    }

    Connections {
        target: HikvisionBackup
        function onCamerasLoaded(cameras) {
            if (win.waiting && win.results.length === 0 && !HikvisionBackup.ruleBusy)
                HikvisionBackup.checkStatus(win.camIp);
        }
        function onStatusResult(card) {
            if (card.key !== win.camIp)
                return;
            win.results = [card];
            win.waiting = false;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Text {
            visible: win.waiting && win.results.length === 0
            text: qsTr("Asking the camera...")
            color: "#8899aa"
            font.pixelSize: 12
            Layout.fillWidth: true
        }

        SetupCheckList {
            Layout.fillWidth: true
            Layout.fillHeight: true
            results: win.results
            openKey: win.camIp
            busy: win.waiting || HikvisionBackup.ruleBusy
            onRecheck: win.run()
        }
    }
}
