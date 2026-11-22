import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property bool finished: false
    property string failure: ""
    property int reader: -1
    property bool started: false
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 1 }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
        onReadyChanged: if (ready) root.run()
    }
    function run(): void {
        if (started) return;
        started = true;
        const draft = store.draft([["settings", "n"]]);
        draft.values.settings.n = 5;
        draft.apply(function (result) {
            if (result.status !== "failed") failure = "apply did not fail";
            draft.discard(); // The user discards the failed page.
            reader = store.values.settings.n;
            finished = true;
        });
    }
    Component.onCompleted: if (store.ready) run()
}
