import QtQuick 2.11
import QtQuick.Controls 2.4
import CCTV_Viewer.Themes 1.0
import CCTV_Viewer.Hikvision 1.0

// The "Backup / Restore" button next to a layout in Presets. It only says it was clicked; the dialog is opened by SideBar.
Button {
    id: btn

    property string layoutUid: ""
    property int backupCount: layoutUid !== "" ? LayoutBackups.count(layoutUid) : 0
    signal activated()

    implicitHeight: 28
    implicitWidth: 112
    text: qsTr("Backup / Restore")

    Connections {
        target: LayoutBackups
        function onChanged() { btn.backupCount = btn.layoutUid !== "" ? LayoutBackups.count(btn.layoutUid) : 0; }
    }

    contentItem: Text {
        text: btn.text
        color: btn.hovered ? "#00f5d4" : "#c8d2da"
        font.pixelSize: 11
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: btn.pressed ? "#2a3540" : (btn.hovered ? "#1f2a34" : "transparent")
        radius: 14
        border.color: btn.hovered ? "#00f5d4" : "#2a3540"
        border.width: 1
    }

    onClicked: btn.activated()

    ToolTip.visible: hovered
    ToolTip.delay: Compact.toolTipDelay
    ToolTip.timeout: Compact.toolTipTimeout
    ToolTip.text: backupCount > 0
                  ? qsTr("Save a copy of this layout, or put one of its %1 saved copies back. Only this layout is changed.").arg(backupCount)
                  : qsTr("Save a copy of this layout now. Nothing is saved yet for it.")
}
