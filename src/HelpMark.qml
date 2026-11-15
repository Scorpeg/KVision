import QtQuick 2.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Themes 1.0

// A small round "?" that shows its text when the pointer rests on it.
Rectangle {
    id: mark

    property string tip: ""

    implicitWidth: 16
    implicitHeight: 16
    width: 16
    height: 16
    radius: 8
    color: area.containsMouse ? "#2a3a4a" : "#1a242d"
    border.width: 1
    border.color: area.containsMouse ? "#00f5d4" : "#5a6b7b"

    Text {
        anchors.centerIn: parent
        text: "?"
        color: area.containsMouse ? "#00f5d4" : "#c8d2da"
        font.pixelSize: 11
        font.bold: true
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        ToolTip.visible: containsMouse
        ToolTip.delay: Compact.toolTipDelay
        ToolTip.timeout: 20000
        ToolTip.text: mark.tip
    }
}
