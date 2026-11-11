import QtQuick 2.12
import QtQuick.Layouts 1.3
import QtQuick.Controls 2.13
import "../common"
import "../common/platformutils.js" as PlatformUtils

Dialog {
    id: root
    objectName: "rdm_bookmarks_panel"

    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    width: 660
    height: 460

    property var itemsModel: []
    property string editingId: ""
    property string editName: ""
    property string editNote: ""
    property string editPattern: ""
    property string filterText: ""
    property var anchorItem: null

    function visibleItems() {
        if (!filterText)
            return itemsModel

        var needle = filterText.toLowerCase()
        var result = []

        for (var i = 0; i < itemsModel.length; ++i) {
            var item = itemsModel[i]
            var haystack = ((item.name || "") + " " + (item.pattern || "") + " "
                            + (item.note || "") + " " + (item.connection || "")).toLowerCase()

            if (haystack.indexOf(needle) >= 0)
                result.push(item)
        }

        return result
    }

    function refresh() {
        if (typeof bookmarksManager === "undefined" || !bookmarksManager)
            return

        itemsModel = bookmarksManager.items()
    }

    function positionNearAnchor() {
        if (!anchorItem || !root.parent)
            return

        var point = anchorItem.mapToItem(root.parent, 0, 0)
        var margin = 10

        var targetX = point.x
        var targetY = point.y - root.height - margin

        if (targetY < margin)
            targetY = point.y + anchorItem.height + margin

        var maxX = Math.max(margin, root.parent.width - root.width - margin)
        var maxY = Math.max(margin, root.parent.height - root.height - margin)

        root.x = Math.max(margin, Math.min(targetX, maxX))
        root.y = Math.max(margin, Math.min(targetY, maxY))
    }

    /* Places the panel right above the given anchor item, aligned to it. */
    function openNear(anchorAnchorItem) {
        anchorItem = anchorAnchorItem
        root.open()
        Qt.callLater(root.positionNearAnchor)
    }

    onHeightChanged: positionNearAnchor()
    onWidthChanged: positionNearAnchor()
    onVisibleChanged: {
        if (visible)
            Qt.callLater(root.positionNearAnchor)
    }

    function openBookmark(bookmark) {
        if (typeof connectionsManager === "undefined" || !connectionsManager)
            return

        if (editingId !== "")
            cancelEdit()

        var opened = connectionsManager.openKeysPattern(bookmark.connection,
                                                        bookmark.db,
                                                        bookmark.pattern)

        if (!opened) {
            notification.showError(qsTranslate("RESP",
                "Can't open the bookmark: connection or database is not loaded yet. Expand the connection in the tree first."))
            return
        }

        root.close()
    }

    function startEdit(bookmark) {
        editingId = bookmark.id
        editName = bookmark.name
        editNote = bookmark.note || ""
        editPattern = bookmark.pattern
    }

    function cancelEdit() {
        editingId = ""
        editName = ""
        editNote = ""
        editPattern = ""
    }

    Component.onCompleted: refresh()

    Connections {
        target: bookmarksManager
        ignoreUnknownSignals: true

        function onChanged() { root.refresh() }
    }

    background: Rectangle {
        color: sysPalette.window
        border.color: sysPalette.mid
        border.width: 1
        radius: 6
    }

    contentItem: ColumnLayout {
        spacing: 0

        /* ---------- header (drag handle) ---------- */
        Rectangle {
            id: header

            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: sysPalette.alternateBase
            radius: 6

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 6
                color: parent.color
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: sysPalette.mid
            }

            MouseArea {
                id: dragArea
                anchors.fill: parent
                anchors.rightMargin: 34
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.SizeAllCursor

                property point pressPoint

                onPressed: pressPoint = Qt.point(mouse.x, mouse.y)

                onPositionChanged: {
                    if (!pressed)
                        return

                    root.x = root.x + (mouse.x - pressPoint.x)
                    root.y = root.y + (mouse.y - pressPoint.y)
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 6
                spacing: 8

                Image {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    source: PlatformUtils.getThemeIcon("bookmark.svg")
                }

                BetterLabel {
                    text: qsTranslate("RESP", "Key Path Bookmarks")
                    font.bold: true
                }

                Rectangle {
                    Layout.preferredWidth: countLabel.implicitWidth + 12
                    Layout.preferredHeight: 18
                    radius: 9
                    color: sysPalette.highlight
                    opacity: 0.85

                    BetterLabel {
                        id: countLabel
                        anchors.centerIn: parent
                        font.pixelSize: 11
                        color: sysPalette.highlightedText
                        text: root.itemsModel.length
                    }
                }

                Item { Layout.fillWidth: true }

                BetterLabel {
                    text: qsTranslate("RESP", "drag to move")
                    opacity: 0.45
                    font.pixelSize: 10
                }

                Item {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    z: 2

                    Rectangle {
                        anchors.fill: parent
                        radius: 11
                        color: closeHover.containsMouse ? sysPalette.mid : "transparent"
                    }

                    BetterLabel {
                        anchors.centerIn: parent
                        text: "\u2715"
                        opacity: 0.75
                        font.pixelSize: 12
                    }

                    MouseArea {
                        id: closeHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
            }
        }

        /* ---------- filter ---------- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: "transparent"

            TextField {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.topMargin: 6
                anchors.bottomMargin: 2
                placeholderText: qsTranslate("RESP", "Filter bookmarks by name, pattern or note...")
                text: root.filterText
                selectByMouse: true
                onTextChanged: root.filterText = text
            }
        }

        /* ---------- list ---------- */
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: bookmarksView
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                clip: true
                spacing: 4
                model: root.visibleItems()

                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    id: delegateRoot

                    width: bookmarksView.width
                    height: delegateColumn.implicitHeight + 14
                    radius: 4

                    property bool isEditing: root.editingId === modelData.id
                    property bool isHovered: rowHover.containsMouse

                    color: isEditing ? sysPalette.alternateBase
                                     : (isHovered ? sysPalette.alternateBase : "transparent")
                    border.width: isHovered || isEditing ? 1 : 0
                    border.color: isEditing ? sysPalette.highlight : sysPalette.mid

                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: Qt.PointingHandCursor
                        enabled: !delegateRoot.isEditing
                        onClicked: root.openBookmark(modelData)
                    }

                    ColumnLayout {
                        id: delegateColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 7
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            /* ----- text block: shrinkable, wraps ----- */
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 0
                                spacing: 2

                                BetterLabel {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    wrapMode: Text.WrapAnywhere
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    font.bold: true
                                    text: modelData.name || ""
                                }

                                BetterLabel {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    visible: !!modelData.note
                                    wrapMode: Text.WrapAnywhere
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    opacity: 0.75
                                    font.pixelSize: 11
                                    text: modelData.note || ""
                                }

                                BetterLabel {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    wrapMode: Text.WrapAnywhere
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    font.family: "Consolas"
                                    font.pixelSize: 11
                                    color: sysPalette.highlight
                                    text: modelData.pattern || ""
                                }

                                BetterLabel {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    elide: Text.ElideRight
                                    opacity: 0.55
                                    font.pixelSize: 10
                                    text: (modelData.connection || "?") + "  \u00b7  db" + modelData.db
                                }
                            }

                            /* ----- actions: fixed size, 2 x 2 ----- */
                            ColumnLayout {
                                Layout.alignment: Qt.AlignTop | Qt.AlignRight
                                Layout.fillWidth: false
                                spacing: 4

                                RowLayout {
                                    spacing: 4

                                    BetterButton {
                                        Layout.preferredWidth: 58
                                        text: qsTranslate("RESP", "Open")
                                        enabled: !delegateRoot.isEditing
                                        onClicked: root.openBookmark(modelData)
                                    }

                                    BetterButton {
                                        Layout.preferredWidth: 58
                                        text: qsTranslate("RESP", "Edit")
                                        enabled: !delegateRoot.isEditing
                                        onClicked: root.startEdit(modelData)
                                    }
                                }

                                RowLayout {
                                    spacing: 4

                                    BetterButton {
                                        Layout.preferredWidth: 28
                                        text: "\u2191"
                                        enabled: index > 0 && !delegateRoot.isEditing
                                        onClicked: bookmarksManager.moveBookmark(modelData.id, -1)
                                    }

                                    BetterButton {
                                        Layout.preferredWidth: 28
                                        text: "\u2193"
                                        enabled: index < root.visibleItems().length - 1 && !delegateRoot.isEditing
                                        onClicked: bookmarksManager.moveBookmark(modelData.id, 1)
                                    }

                                    BetterButton {
                                        Layout.preferredWidth: 60
                                        text: qsTranslate("RESP", "Delete")
                                        enabled: !delegateRoot.isEditing
                                        onClicked: bookmarksManager.removeBookmark(modelData.id)
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            visible: delegateRoot.isEditing
                            color: sysPalette.mid
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            visible: delegateRoot.isEditing
                            columns: 3
                            columnSpacing: 8
                            rowSpacing: 4

                            BetterLabel {
                                text: qsTranslate("RESP", "Name")
                                Layout.preferredWidth: 46
                            }

                            TextField {
                                id: nameField
                                Layout.fillWidth: true
                                placeholderText: qsTranslate("RESP", "Custom name")
                                text: root.editName
                                selectByMouse: true
                            }

                            Item { Layout.preferredWidth: 1 }

                            BetterLabel {
                                text: qsTranslate("RESP", "Note")
                                Layout.preferredWidth: 46
                            }

                            TextField {
                                id: noteField
                                Layout.fillWidth: true
                                placeholderText: qsTranslate("RESP", "Optional note")
                                text: root.editNote
                                selectByMouse: true
                            }

                            Item { Layout.preferredWidth: 1 }

                            BetterLabel {
                                text: qsTranslate("RESP", "Pattern")
                                Layout.preferredWidth: 46
                            }

                            TextField {
                                id: patternField
                                Layout.fillWidth: true
                                placeholderText: "A0_CLOUDFLARE5S:*"
                                text: root.editPattern
                                selectByMouse: true
                            }

                            Item { Layout.preferredWidth: 1 }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: delegateRoot.isEditing
                            spacing: 6

                            Item { Layout.fillWidth: true }

                            BetterButton {
                                Layout.preferredWidth: 60
                                text: qsTranslate("RESP", "Save")
                                onClicked: {
                                    var result = bookmarksManager.updateBookmark(modelData.id,
                                                                                 nameField.text,
                                                                                 noteField.text,
                                                                                 patternField.text)

                                    if (result === "")
                                        root.cancelEdit()
                                }
                            }

                            BetterButton {
                                Layout.preferredWidth: 60
                                text: qsTranslate("RESP", "Cancel")
                                onClicked: root.cancelEdit()
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 4
                visible: root.visibleItems().length === 0

                BetterLabel {
                    Layout.alignment: Qt.AlignHCenter
                    opacity: 0.6
                    text: root.itemsModel.length === 0
                          ? qsTranslate("RESP", "No bookmarks yet")
                          : qsTranslate("RESP", "No bookmark matches the filter")
                }

                BetterLabel {
                    Layout.alignment: Qt.AlignHCenter
                    opacity: 0.45
                    font.pixelSize: 11
                    text: qsTranslate("RESP", "Use the bookmark button in the namespace context menu to add one")
                }
            }
        }

        /* ---------- footer ---------- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            color: sysPalette.alternateBase
            radius: 6

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 6
                color: parent.color
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: sysPalette.mid
            }

            BetterLabel {
                anchors.fill: parent
                anchors.leftMargin: 10
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                opacity: 0.5
                font.pixelSize: 10
                text: qsTranslate("RESP", "Click a bookmark to open it \u00b7 Edit to rename or add a note")
            }
        }
    }
}
