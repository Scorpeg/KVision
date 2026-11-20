import QtQuick 2.11
import QtQuick.Window 2.11
import QtQuick.Controls 2.4
import QtQuick.Layouts 1.11
import CCTV_Viewer.Hikvision 1.0

Window {
    id: smartWindow
    width: 760
    height: 560
    title: qsTr("Smart search")
    color: "#10161c"
    flags: Qt.Window | Qt.WindowTitleHint | Qt.WindowCloseButtonHint | Qt.WindowMinimizeButtonHint

    property var playback: null
    property int token: 0
    property bool busy: false
    property string note: ""
    property var results: []

    function openForCurrent() {
        show()
        raise()
        requestActivate()
    }

    property var searchRecorder: null

    function runSearch() {
        var recorder = playback && playback.recorderForSearch ? playback.recorderForSearch() : null
        if (!recorder || !recorder.ip) {
            note = qsTr("Select a recorder or a camera in the list on the left.")
            results = []
            return
        }
        searchRecorder = recorder
        if (!humanBox.checked && !vehicleBox.checked && !faceBox.checked && !otherBox.checked
                && !lineBox.checked && !intrusionBox.checked && !entranceBox.checked) {
            note = qsTr("Choose at least one: human, vehicle, face, other, or an event.")
            return
        }
        var day = new Date(playback.currentDate.getTime())
        day.setHours(0, 0, 0, 0)
        var end = new Date(day.getTime() + 86400000 - 1000)
        var channels = []
        var one = playback.selectedSmartCamera
        if (one && one.ip === recorder.ip && one.channelId) {
            channels.push({ channelId: one.channelId, name: one.name || "" })
        } else {
            var cams = recorder.cameras || []
            for (var c = 0; c < cams.length; ++c) {
                if (!cams[c] || !cams[c].channelId)
                    continue
                channels.push({
                    channelId: cams[c].channelId,
                    name: cams[c].customName || cams[c].name || ""
                })
            }
        }
        token = token + 1
        busy = true
        note = channels.length === 1 ? qsTr("Searching the camera…") : qsTr("Searching the recorder…")
        results = []
        HikvisionISAPI.searchSmartTargets(recorder, channels, day, end,
                                           humanBox.checked, vehicleBox.checked, faceBox.checked, otherBox.checked,
                                           lineBox.checked, intrusionBox.checked, entranceBox.checked, token)
    }

    function playHit(hit) {
        if (!playback || !hit)
            return
        var recorder = searchRecorder
        if (!recorder || !recorder.ip)
            return
        var when = new Date(hit.timeMs - 5000)
        var channel = hit.channelId || playback.channelId
        playback.openCameraAt(recorder, {
            channelId: channel,
            name: hit.cameraName || ""
        }, when.getTime())
        playback.raise()
        playback.requestActivate()
    }

    Connections {
        target: HikvisionISAPI
        onSmartSearchFinished: {
            if (token !== smartWindow.token)
                return
            smartWindow.busy = false
            smartWindow.results = hits
            smartWindow.note = note
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Text {
            Layout.fillWidth: true
            color: "#c5d0d8"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            text: {
                var chosen = playback ? playback.smartSelectionKey : ""
                var day = (playback && playback.formatDay) ? playback.formatDay(playback.currentDate) : ""
                var one = playback ? playback.selectedSmartCamera : null
                var rec = playback ? playback.selectedRecorder : null
                var who = qsTr("Every camera on this recorder")
                if (chosen.indexOf("cam:") === 0 && one && one.channelId && (!rec || one.ip === rec.ip))
                    who = one.name || qsTr("Camera %1").arg(one.channelId)
                return who + (day !== "" ? ("  ·  " + day) : "")
            }
        }

        Text {
            color: "#8898a6"
            font.pixelSize: 11
            font.bold: true
            text: qsTr("Target Type")
        }

        RowLayout {
            spacing: 14
            CheckBox {
                id: humanBox
                checked: false
                text: qsTr("Human")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: humanBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: humanBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            CheckBox {
                id: vehicleBox
                checked: false
                text: qsTr("Vehicle")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: vehicleBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: vehicleBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            CheckBox {
                id: faceBox
                checked: false
                text: qsTr("Face")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: faceBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: faceBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            CheckBox {
                id: otherBox
                checked: false
                text: qsTr("Other")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: otherBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: otherBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                text: smartWindow.busy ? qsTr("Searching…") : qsTr("Search")
                enabled: !smartWindow.busy
                implicitHeight: 30
                onClicked: smartWindow.runSearch()
                contentItem: Text {
                    text: parent.text
                    color: "white"
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: parent.down ? "#d66600" : (parent.hovered ? "#ff8c00" : "#ff7a00")
                    opacity: parent.enabled ? 1 : 0.45
                }
            }
        }

        Text {
            color: "#8898a6"
            font.pixelSize: 11
            font.bold: true
            text: qsTr("Event Type")
        }

        RowLayout {
            spacing: 14
            CheckBox {
                id: lineBox
                checked: false
                text: qsTr("Line crossing")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: lineBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: lineBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            CheckBox {
                id: intrusionBox
                checked: false
                text: qsTr("Intrusion")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: intrusionBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: intrusionBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            CheckBox {
                id: entranceBox
                checked: false
                text: qsTr("Region entrance")
                palette.highlight: "#00f5d4"
                contentItem: Text {
                    text: entranceBox.text
                    color: "white"
                    font.pixelSize: 12
                    leftPadding: entranceBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Text {
            Layout.fillWidth: true
            color: "#ff7a00"
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            text: smartWindow.note
            visible: text.length > 0
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: smartWindow.results.length === 0 || smartWindow.results[0].image
            cellWidth: 188
            cellHeight: 248
            clip: true
            model: visible ? smartWindow.results : []
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                width: grid.cellWidth - 8
                height: grid.cellHeight - 8
                color: hitArea.containsMouse ? "#24313d" : "#0f151b"
                radius: 4
                border.color: "#2a3540"

                Column {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 4

                    Image {
                        width: parent.width
                        height: parent.height - 72
                        source: modelData.image ? ("file://" + modelData.image) : ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                    }
                    Text {
                        width: parent.width
                        height: 68
                        wrapMode: Text.WordWrap
                        clip: true
                        color: "#d5dee6"
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                        text: {
                            var kind = modelData.kind === "human" ? qsTr("Human")
                                     : modelData.kind === "vehicle" ? qsTr("Vehicle")
                                     : modelData.kind === "face" ? qsTr("Face")
                                     : modelData.kind === "other" ? qsTr("Other")
                                     : modelData.kind === "event" ? qsTr("Event")
                                     : (modelData.kind || "")
                            var lines = []
                            if (modelData.cameraName)
                                lines.push(modelData.cameraName)
                            var mid = kind
                            if (modelData.detail)
                                mid = mid ? (mid + "  ·  " + modelData.detail) : modelData.detail
                            if (mid)
                                lines.push(mid)
                            if (modelData.timeMs) {
                                var when = new Date(modelData.timeMs)
                                lines.push(Qt.formatTime(when, "h:mm:ss AP"))
                            }
                            return lines.length ? lines.join("\n") : (modelData.label || "")
                        }
                    }
                }

                MouseArea {
                    id: hitArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: smartWindow.playHit(modelData)
                }
            }
        }

        ListView {
            id: eventList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: smartWindow.results.length > 0 && !smartWindow.results[0].image
            clip: true
            spacing: 4
            model: visible ? smartWindow.results : []
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                width: eventList.width
                height: eventLabel.implicitHeight + 16
                color: eventHit.containsMouse ? "#24313d" : "#0f151b"
                radius: 4
                border.color: "#2a3540"

                Text {
                    id: eventLabel
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.topMargin: 8
                    wrapMode: Text.WordWrap
                    color: "#d5dee6"
                    font.pixelSize: 12
                    text: modelData.label || ""
                }

                MouseArea {
                    id: eventHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: smartWindow.playHit(modelData)
                }
            }
        }
    }
}
