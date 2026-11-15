import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Layouts 1.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Core 1.0
import CCTV_Viewer.Hikvision 1.0
import CCTV_Viewer.Utils 1.0

// The result of "Camera clock and firmware > Check all": one card per camera, filled in as each camera answers.
// It only reads. Nothing on a camera is changed.
Window {
    id: win

    width: 780
    height: 520
    minimumWidth: 520
    minimumHeight: 300
    color: "#0f151b"
    title: qsTr("Camera clock and firmware")
    flags: Qt.Window | Qt.WindowStaysOnTopHint

    // The cameras to list: { name, ip }. Set before run().
    property var cameras: []
    property bool running: false
    property string progressText: ""

    ListModel { id: rows }

    function stateColor(state) {
        return state === "ok" ? "#14a24c" : (state === "warn" ? "#e0a040" : (state === "error" ? "#e0241f" : (state === "unavailable" ? "#667788" : "#8899aa")));
    }

    function stateMark(state) {
        return state === "ok" ? "\u2713" : (state === "warn" ? "!" : (state === "error" ? "\u2717" : (state === "unavailable" ? "\u2013" : "\u2026")));
    }

    function count(state) {
        var n = 0;
        for (var i = 0; i < rows.count; ++i)
            if (rows.get(i).status === state)
                ++n;
        return n;
    }

    // Lists every camera as "waiting" and asks them one after the other (read only).
    function run() {
        if (HikvisionBackup.ruleBusy)
            return;
        rows.clear();
        for (var i = 0; i < win.cameras.length; ++i)
            rows.append({ "name": win.cameras[i].name, "ip": win.cameras[i].ip, "status": "waiting", "message": qsTr("waiting...") });
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
        function onHealthAllItem(ip, state, message) {
            for (var i = 0; i < rows.count; ++i) {
                if (rows.get(i).ip === ip && rows.get(i).status === "waiting") {
                    rows.set(i, { "name": rows.get(i).name, "ip": ip, "status": state, "message": message });
                    break;
                }
            }
        }
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
                      : qsTr("%1 camera(s): %2 fine, %3 need a look, %4 without the feature or not read")
                            .arg(rows.count).arg(win.count("ok")).arg(win.count("warn") + win.count("error"))
                            .arg(win.count("unavailable") + win.count("waiting"))
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

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: rows
            ScrollBar.vertical: ScrollBar { }

            delegate: Rectangle {
                width: list.width - 12
                implicitHeight: body.implicitHeight + 16
                height: implicitHeight
                color: "#1c242c"
                radius: 6
                border.color: win.stateColor(model.status)
                border.width: 1

                RowLayout {
                    id: body
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 10

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        width: 24
                        height: 24
                        radius: 12
                        color: win.stateColor(model.status)
                        Text {
                            anchors.centerIn: parent
                            text: win.stateMark(model.status)
                            color: "white"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Text {
                            text: model.name + "  (" + model.ip + ")"
                            color: "white"
                            font.pixelSize: 13
                            font.bold: true
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Text {
                            textFormat: Text.StyledText
                            text: Utils.healthRich(model.status === "unavailable" ? qsTr("Not available: %1").arg(model.message)
                                  : (model.status === "error" ? qsTr("Problem: %1").arg(model.message) : model.message))
                            color: model.status === "warn" ? "#e0a040" : (model.status === "error" ? "#ff6666" : "#c8d2dc")
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }
}
