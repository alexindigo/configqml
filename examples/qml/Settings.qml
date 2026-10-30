// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQml
import ConfigQml 1.0

ConfigStore {
    id: store
    autoSave: false
    schema: ConfigSchema {
        ConfigGroup {
            key: "bar"
            ConfigValue {
                key: "outputs"
                valueType: ConfigValue.StringList
                defaultValue: []
            }
        }
    }
    sources: [MemorySource { layerId: "user"; sourceId: "preferences"; writable: true }]
    readonly property var cfg: values
    onStatusChanged: {
        if (status === ConfigStore.Error) {
            console.error(error)
            Qt.exit(1)
        }
        if (!ready)
            return
        cfg.bar.outputs.push("DP-1")
        cfg.bar.outputs.push("HDMI-A-1")
        if (cfg.bar.outputs.length !== 2) {
            Qt.exit(2)
            return
        }
        close(function(result) {
            if (result.status !== "saved" || cfg.bar.outputs.join(",") !== "DP-1,HDMI-A-1") {
                Qt.exit(3)
                return
            }
            console.log("CONFIGQML_QML_OK: " + cfg.bar.outputs.join(","))
            Qt.quit()
        })
    }
}
