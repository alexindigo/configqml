import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    property bool stopped: false
    property bool deadCallback: false
    property string token: ""
    property var capture: null
    property var popup: null
    property bool signalSeen: false
    readonly property bool ready: store.ready
    readonly property int observed: store.ready ? store.values.choice : -1
    property ConfigStore store: ConfigStore {
        id: store
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "choice"; valueType: ConfigValue.Integer; defaultValue: 0 }
            ConfigValue { key: "other"; valueType: ConfigValue.Integer; defaultValue: 0 }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/blocked/user.json" }]
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
        onCompleted: function(result) {
            if (result.operation === "saveGroup" && result.status === "failed") {
                if (result.error.code !== "lock-failed" || result.attemptNumber !== "1"
                        || store.groupInfo(root.token).state !== "stopped") root.failure = "wrong-save-fault";
                root.stopped = true;
            }
            if (result.operation === "retryGroup") {
                root.signalSeen = true;
                if (store.groupInfo(root.token).state !== "succeeded") root.failure = "group-cache-order";
            }
        }
    }
    function run() {
        store.retainGroup({requestKey:"frozen",paths:[["choice"]],edits:[
            {path:["choice"],present:true,value:1}
        ],dependencies:[]}, function(result) {
            if (result.status !== "retained" || root.observed !== 1) {
                root.failure = "group-capture";
                return;
            }
            root.capture = result;
            root.token = result.groupToken;
            const editor = store.draft([["choice"]]);
            editor.values.choice = 3;
            const before = store.info(["choice"]);
            store.retainGroup({requestKey:"frozen",paths:[["choice"]],edits:[
                {path:["choice"],present:true,value:1}
            ],dependencies:[]}, function(same) {
                const after = store.info(["choice"]);
                if (same.groupToken !== root.token || same.capturePrefix !== root.capture.capturePrefix
                        || after.latestInput !== before.latestInput
                        || after.pathGeneration !== before.pathGeneration
                        || !editor.dirty || editor.conflicted || editor.values.choice !== 3) {
                    root.failure = "idempotent-retain-mutated-editor";
                    return;
                }
                editor.detach();
                store.values.choice = 9;
                root.popup = store.scope([], true);
                root.popup.saveGroup(root.token, function() { root.deadCallback = true; });
                root.popup.detach();
            });
        });
    }
    function retry() {
        store.retryGroup(token, function(result) {
            if (result.status !== "saved" || result.attemptNumber !== "2" || !root.signalSeen
                    || result.capturePrefix !== root.capture.capturePrefix || root.observed !== 9 || root.deadCallback) {
                root.failure = "group-frozen-retry";
                return;
            }
            store.releaseGroup(root.token, function(released) {
                const attempts = store.groupInfo(root.token).attempts;
                if (released.status !== "released" || attempts.length !== 2
                        || attempts[0].status !== "failed" || attempts[0].error.code !== "lock-failed"
                        || attempts[1].status !== "saved")
                    root.failure = "group-history";
                else root.finished = true;
            });
        });
    }
}
