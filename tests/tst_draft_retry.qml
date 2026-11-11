import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property string stage: "idle"
    property string action: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property string observed: store.ready ? store.values.settings.label : ""
    property var editor: null
    property var accepted: null
    property string admittedPrefix: ""
    property ConfigStore store: ConfigStore {
        autoSave: false
        evidenceDirectory: root.directory + "/evidence"
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "label"; valueType: ConfigValue.String; defaultValue: "before" }
            }
        }
        sources: [JsonFileSource {
            sourceId: "user"
            path: root.directory + "/user.json"
            writable: true
            conflictPolicy: JsonFileSource.StrictPendingRebase
        }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
        return ok;
    }
    function run(): void {
        store.values.settings.label = "before";
        store.save(function(result) {
            if (!check(result.status === "saved", "baseline publication")) return;
            stage = "permission";
            action = "block";
        });
    }
    function faultReady(): void {
        action = "";
        const scope = store.scope(["settings"]);
        editor = scope.draft([["label"]]);
        editor.values.label = "after";
        if (!check(observed === "before" && editor.values.label === "after", "detached scoped draft")) return;
        editor.apply(function(result) {
            if (!check(result.status === "failed" && result.error.code === "stage-open-failed"
                       && observed === "after" && editor.dirty && !editor.conflicted,
                       "failed publication retains own admitted draft")) return;
            accepted = store.info(["settings", "label"]);
            admittedPrefix = result.inputPrefix;
            stage = "retry";
            action = "repair";
        });
    }
    function retryReady(): void {
        action = "";
        editor.apply(function(result) {
            const current = store.info(["settings", "label"]);
            if (!check(result.status === "saved" && result.inputPrefix === admittedPrefix
                       && current.intentRevision === accepted.intentRevision
                       && current.pathGeneration === accepted.pathGeneration
                       && observed === "after" && editor.values.label === "after"
                       && !editor.dirty && !editor.conflicted && !editor.busy,
                       "same scoped draft Retry without duplicate intent")) return;
            finished = true;
        });
    }
    function runRestart(): void {
        if (check(observed === "after", "cold consumer sees repaired Apply")) finished = true;
    }
}
