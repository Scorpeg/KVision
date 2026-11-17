import QtQuick 2.11
import QtQuick.Controls 2.4
import QtQuick.Layouts 1.11

// A list of tick boxes (used for both the "show" and the "leave out" filters of the Detection window) that stays open while several are ticked (a Menu closes after every click). It closes on a click outside it
// or Escape. "choices" is a list of {label, key}; "ticked" maps key -> bool and is only read: a tick or the Clear button asks the
// owner to change it through tickChanged(key, checked) and cleared(), so the owner keeps the one copy of what is ticked.
Popup {
    id: pop

    property var choices: []
    property var ticked: ({})
    readonly property int tickedCount: {
        var n = 0;
        for (var k in pop.ticked)
            if (pop.ticked[k])
                ++n;
        return n;
    }

    signal tickChanged(string key, bool checked)
    signal cleared()
    // Sent once when the list closes after something was ticked, so the owner can act on the finished choice
    // instead of after every tick (reloading a long list for each tick is slow).
    signal applied()

    property bool dirty: false
    onClosed: {
        if (dirty) {
            dirty = false;
            applied();
        }
    }

    padding: 6
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // Modal without dimming: that is what makes a click anywhere else in the window close it (and only close it).
    modal: true
    dim: false
    focus: true

    background: Rectangle {
        color: "#0f151b"
        border.color: "#ff7a00"
        radius: 6
    }

    contentItem: ColumnLayout {
        spacing: 2

        Repeater {
            model: pop.choices
            delegate: CheckBox {
                id: box
                Layout.fillWidth: true
                text: modelData.label
                // A click replaces the box's own "checked" binding, so the box would stop following the list. The list is the one source of
                // truth: every change to it, and every time the popup opens, sets the box from it again.
                property bool wanted: !!pop.ticked[modelData.key]
                onWantedChanged: box.checked = wanted
                Component.onCompleted: box.checked = wanted
                Connections {
                    target: pop
                    function onAboutToShow() { box.checked = box.wanted }
                }
                onToggled: {
                    pop.dirty = true;
                    pop.tickChanged(modelData.key, box.checked);
                }
                contentItem: Text {
                    text: box.text
                    color: box.hovered ? "#ff7a00" : "white"
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: box.indicator.width + box.spacing
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#2a3540"
        }

        Button {
            id: clearBtn
            Layout.fillWidth: true
            text: qsTr("Clear")
            enabled: pop.tickedCount > 0
            onClicked: {
                pop.dirty = true;
                pop.cleared();
            }
            contentItem: Text {
                text: clearBtn.text
                color: clearBtn.enabled ? "white" : "#8898a6"
                font.pixelSize: 12
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: !clearBtn.enabled ? "#1c242c" : (clearBtn.hovered ? "#3a4550" : "#2a3540")
                radius: 4
            }
        }
    }
}
