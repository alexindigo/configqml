import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property int observed: store.ready ? store.values.good.n : -1
    property ConfigStore store: ConfigStore {
        id: store
        autoSave: false
        evidenceDirectory: root.directory + "/evidence"
        schema: ConfigSchema {
            ConfigGroup { key: "bad"; ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 } }
            ConfigGroup { key: "good"; ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 } }
        }
        sources: [
            JsonFileSource {
                layerId: "user"; sourceId: "bad"; prefix: ["bad"]; path: root.directory + "/bad.json"
                startupErrorPolicy: JsonFileSource.SkipProjectionRetainError
            },
            JsonFileSource {
                layerId: "user"; sourceId: "good"; prefix: ["good"]; path: root.directory + "/good.json"
            }
        ]
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        if (!store.sourceStates[0].quarantined || store.sourceStates[0].load !== "parse_error"
                || store.sourceStates[0].presence !== "present") {
            failure = "startup-metadata";
            return;
        }
        store.values.good.n = 8;
        store.save(function(result) {
            if (result.status !== "saved" || root.observed !== 8 || !store.sourceStates[0].quarantined)
                root.failure = "startup-unrelated-route";
            else
                root.finished = true;
        });
    }
    function runRestart() {
        if (observed !== 8 || !store.sourceStates[0].quarantined) failure = "startup-restart";
        else finished = true;
    }
}
