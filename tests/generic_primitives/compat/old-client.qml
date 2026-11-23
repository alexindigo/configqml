import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property bool finished: false
    property string failure: ""
    property bool started: false
    property var scope: null
    property var draft: null
    readonly property var values: store.values
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "count"; valueType: ConfigValue.Integer; defaultValue: 1 }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
        onReadyChanged: if (ready) root.run()
    }
    function check(ok: bool, reason: string): bool {
        if (!ok && failure === "") failure = reason;
        return ok;
    }
    function run(): void {
        if (started) return;
        started = true;
        scope = store.scope(["settings"]);
        scope.values.count = 4;
        scope.save(function(saved) {
            if (!check(saved.status === "saved" && scope.values.count === 4, "scoped save")) return;
            draft = store.draft([["settings"]]);
            draft.values.settings.count = 9;
            if (!check(values.settings.count === 4, "draft isolation")) return;
            if (!check(draft.discard() && draft.values.settings.count === 4, "Discard")) return;
            draft.stageReset([["settings", "count"]], function(staged) {
                if (!check(staged.status === "staged" && draft.values.settings.count === 1,
                           "staged Reset")) return;
                draft.apply(function(applied) {
                    if (!check(applied.status === "saved" && values.settings.count === 1,
                               "Reset Apply")) return;
                    store.close(function(closed) {
                        if (check(closed.status === "nothing_to_save", "close")) finished = true;
                    });
                });
            });
        });
    }
    Component.onCompleted: if (store.ready) run()
}
