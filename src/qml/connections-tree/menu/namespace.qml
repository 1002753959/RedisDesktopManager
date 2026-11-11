import QtQuick 2.3
import QtQuick.Layouts 1.1
import QtQuick.Controls 1.4
import "."
import "./../../common/platformutils.js" as PlatformUtils

InlineMenu {
    id: root

    callbacks: {
        "copy": function() {
            var result = styleData.value["full_path"]

            if (result) {
                qmlUtils.copyToClipboard(result + ":*")
            }
        },
        "bookmark": function() {
            if (typeof bookmarksManager === "undefined" || !bookmarksManager)
                return

            var context = connectionsManager.bookmarkContext(styleData.index)

            if (!context || !context.pattern) {
                notification.showError(qsTranslate("RESP", "Can't add bookmark for this item"))
                return
            }

            bookmarksManager.addBookmark(context.pattern, context.connection,
                                         context.db, context.name, "")
        },
        "copy_full_path": function() {
            var result = styleData.value["full_path"]

            if (result) {
                qmlUtils.copyToClipboard(result)
            }
        },
    }

    model: {
        if (styleData.value["locked"] === true) {
            return [
                        {
                            'icon': PlatformUtils.getThemeIcon("offline.svg"), 'event': 'cancel', "help": qsTranslate("RESP","Disconnect"),
                        },
                    ]
        } else {
            return [
                {'icon': PlatformUtils.getThemeIcon("refresh.svg"), "event": "reload", "help": qsTranslate("RESP","Reload Namespace"), "shortcut": "Ctrl+R"},
                {'icon': PlatformUtils.getThemeIcon("load_all.svg"), "event": "load_all", "help": qsTranslate("RESP","Load all keys (ignore keys loading limit)")},
                {'icon': PlatformUtils.getThemeIcon("add.svg"), 'event': 'add_key', "help": qsTranslate("RESP","Add New Key")},
                {'icon': PlatformUtils.getThemeIcon("copy.svg"), "callback": "copy", "help": qsTranslate("RESP","Copy Namespace Pattern"), "shortcut": "Ctrl+C"},
                {'icon': PlatformUtils.getThemeIcon("bookmark.svg"), "callback": "bookmark", "help": qsTranslate("RESP","Add Bookmark"), "shortcut": "Ctrl+B"},
                {'icon': PlatformUtils.getThemeIcon("memory_usage.svg"), "event": "analyze_memory_usage", "help": qsTranslate("RESP","Analyze Used Memory")},
                {'icon': PlatformUtils.getThemeIcon("delete.svg"), "event": "delete", "help": qsTranslate("RESP","Delete Namespace"), "shortcut": "D"},
            ]
        }
    }
}
