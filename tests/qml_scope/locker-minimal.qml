// SPDX-License-Identifier: LGPL-3.0-or-later
// Standalone, inert Quickshell reproduction from the Atmosphera locker report.
import QtQuick
import ConfigQml 1.0
import Quickshell

ShellRoot {
    id: root
    readonly property string witness: "outer"
    Timer { interval: 1500; running: true; onTriggered: Qt.exit(42) }
    Loader {
        sourceComponent: Component {
            ConfigStore {
                autoSave: false
                readonly property var resolved: root
                Component.onCompleted: {
                    console.log("ROOT_RESOLUTION", resolved === null, resolved?.witness ?? "missing");
                    Qt.callLater(Qt.quit);
                }
            }
        }
    }
}
