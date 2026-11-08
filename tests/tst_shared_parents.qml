import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property var cfg: store.values
    property ConfigStore store: ConfigStore {
        autoSave: false
        evidenceDirectory: root.directory + "/evidence"
        schema: ConfigSchema {
            ConfigGroup { key: "general"
                ConfigValue { key: "scaleRatio"; valueType: ConfigValue.Real; defaultValue: 1.0 }
            }
            ConfigGroup { key: "bar"
                ConfigValue { key: "position"; valueType: ConfigValue.String; defaultValue: "top" }
            }
            ConfigGroup { key: "wallpaper"
                ConfigValue { key: "directory"; valueType: ConfigValue.String; defaultValue: "" }
            }
        }
        sources: [
            JsonFileSource {
                sourceId: "general"; prefix: ["general"]
                path: root.directory + "/settings/general.json"
                conflictPolicy: JsonFileSource.StrictPendingRebase
            },
            JsonFileSource {
                sourceId: "bar"; prefix: ["bar"]
                path: root.directory + "/settings/bar.json"
                conflictPolicy: JsonFileSource.StrictPendingRebase
            },
            JsonFileSource {
                sourceId: "wallpaper"; prefix: ["wallpaper"]
                path: root.directory + "/settings/wallpaper.json"
                conflictPolicy: JsonFileSource.StrictPendingRebase
            }
        ]
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        store.retainGroup({requestKey: "first-use",
            paths: [["general", "scaleRatio"], ["bar", "position"], ["wallpaper", "directory"]],
            edits: [
                {path: ["general", "scaleRatio"], present: true, value: 1.74},
                {path: ["bar", "position"], present: true, value: "left"},
                {path: ["wallpaper", "directory"], present: true, value: "/owner-wallpapers"}
            ], dependencies: []}, function(capture) {
                if (capture.status !== "retained") { root.failure = "first-use-capture"; return; }
                store.saveGroup(capture.groupToken, function(result) {
                    if (result.status !== "saved" || result.publications.length !== 3) {
                        root.failure = "first-use-save:" + result.error.code; return;
                    }
                    for (const fact of result.publications) {
                        if (!fact.success) { root.failure = "first-use-route"; return; }
                    }
                    root.check();
                    if (root.failure) return;
                    store.close(function(closed) {
                        if (closed.status !== "nothing_to_save") root.failure = "first-use-close";
                        else root.finished = true;
                    });
                });
            });
    }
    function check() {
        if (cfg.general.scaleRatio !== 1.74 || cfg.bar.position !== "left"
                || cfg.wallpaper.directory !== "/owner-wallpapers") failure = "first-use-consumer";
    }
    function runRestart() { check(); finished = !failure; }
}
