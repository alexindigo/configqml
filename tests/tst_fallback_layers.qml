import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property int observedA: store.ready ? store.values.a : -1
    readonly property int observedB: store.ready ? store.values.b : -1
    property ConfigStore store: ConfigStore {
        id: store
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "a"; valueType: ConfigValue.Integer; defaultValue: 0 }
            ConfigValue { key: "b"; valueType: ConfigValue.Integer; defaultValue: 0 }
        }
        sources: [
            JsonFileSource {
                layerId: "legacy"; sourceId: "legacy"; path: root.directory + "/legacy.json"
                writable: false; fallbackFor: "user"
            },
            JsonFileSource {
                layerId: "user"; sourceId: "user"; path: root.directory + "/user.json"
                writable: true
            }
        ]
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        if (observedA !== 1 || observedB !== 2) {
            failure = "fallback-absent-and-empty";
            return;
        }
        store.values.a = 3;
        store.save(function(result) {
            if (result.status !== "saved" || root.observedA !== 3 || root.observedB !== 0) {
                root.failure = "fallback-nonempty-suppresses-whole-source";
                return;
            }
            store.reset([], "user");
            store.save(function(reset) {
                if (reset.status !== "saved" || root.observedA !== 1 || root.observedB !== 2)
                    root.failure = "fallback-reset";
                else
                    root.finished = true;
            });
        });
    }
    function runRestart() {
        if (observedA !== 1 || observedB !== 2) failure = "fallback-restart";
        else finished = true;
    }
}
