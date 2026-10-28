// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    property string directory
    property bool automatic: false
    readonly property var cfg: fileStore.values
    readonly property int count: fileStore.ready ? cfg.bar.items.length : -1
    readonly property string wide: fileStore.ready ? cfg.bar.wide : "loading"
    property var receipt: ({})
    property ConfigStore store: ConfigStore {
        id: fileStore
        autoSave: root.automatic
        autoSaveInterval: 20
        onAutoSaved: function(result) { root.receipt = result }
        schema: ConfigSchema {
            ConfigGroup {
                key: "bar"
                ConfigValue { key: "items"; valueType: ConfigValue.StringList; defaultValue: [] }
                ConfigValue { key: "wide"; valueType: ConfigValue.UInt64; defaultValue: "0" }
            }
            ConfigGroup {
                key: "other"
                ConfigValue { key: "enabled"; valueType: ConfigValue.Boolean; defaultValue: false }
            }
        }
        sources: [
            JsonFileSource {
                layerId: "user"; sourceId: "bar"; prefix: ["bar"]
                path: root.directory + "/bar.json"
                watchMode: JsonFileSource.Live
            },
            JsonFileSource {
                layerId: "user"; sourceId: "other"; prefix: ["other"]
                path: root.directory + "/other.json"
            }
        ]
    }
    function editAndClose() {
        cfg.bar.items.push("one")
        cfg.bar.items.push("two")
        cfg.bar.wide = "18446744073709551615"
        fileStore.close(function(result) { root.receipt = result })
    }
    function seal() { fileStore.close(function(result) { root.receipt = result }) }
    function autoEdit() { cfg.bar.items.push("auto") }
}
