import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property var draft: null
    property bool started: false
    property string failure: ""
    property string observations: ""
    readonly property var current: store.values
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 1 }
        }
        sources: [
            MemorySource { layerId: "legacy"; sourceId: "legacy"; writable: false; data: ({n: 7}) },
            JsonFileSource { layerId: "user"; sourceId: "user"; writable: true; path: root.directory + "/qml.json" }
        ]
        onStatusChanged: {
            if (status === ConfigStore.Error) { root.failure = error; Qt.exit(11); }
            if (ready && !root.started) { root.started = true; Qt.callLater(root.run); }
        }
    }
    function check(ok: bool, message: string): bool {
        if (!ok) { failure = "STAGED_RESET_BASELINE_FAILURE: " + message; Qt.exit(10); }
        return ok;
    }
    function run(): void {
        draft = store.draft([["n"]]);
        let refused = false;
        try { draft.reset(["n"], "user"); } catch (error) { refused = true; }
        if (!check(refused && draft.values.n === 9 && !draft.dirty && current.n === 9,
                   "old draft reset refuses without input")) return;
        observations += "OLD_QML_BASELINE: DRAFT_RESET_REFUSED\n";
        draft.values.n = 1;
        if (!check(draft.values.n === 1 && draft.preview.n === 1 && current.n === 9,
                   "default assignment cannot reveal legacy 7")) return;
        if (!check(draft.discard() && draft.values.n === 9, "discard explicit set")) return;
        draft.values.n = 1;
        draft.apply(function(result) {
            if (!check(result.status === "saved" && current.n === 1,
                       "explicit default is saved, not removed")) return;
            observations += "OLD_QML_BASELINE: DEFAULT_ASSIGNMENT_NOT_REMOVAL\n";
            Qt.quit();
        });
    }
}
