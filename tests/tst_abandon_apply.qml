import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property bool finished: false
    property string failure: ""
    property bool started: false
    property var draft: null
    property int callbacks: 0
    property int boundAtCallback: -1
    property int snapshotAtCallback: -1
    property string abandonStatus: ""
    property string secondStatus: ""
    property string secondCode: ""
    readonly property var values: store.values
    readonly property int bound: store.ready ? values.settings.n : -1
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 1 }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
    }
    signal failedApply()
    function run(): void {
        draft = store.draft([["settings", "n"]]);
        draft.values.settings.n = 5;
        draft.apply(function (result) {
            if (result.status !== "failed") failure = "apply did not fail";
            failedApply();
        });
    }
    function abandon(): void {
        draft.abandonApply(function (result) {
            ++callbacks;
            abandonStatus = result.status;
            boundAtCallback = bound;
            snapshotAtCallback = store.readProjection.state(["settings", "n"]).value;
            draft.abandonApply(function (again) {
                secondStatus = again.status;
                secondCode = again.error ? again.error.code : "";
                finished = true;
            });
        });
    }
}
