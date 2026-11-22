import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property var baseScope: null
    property var runtimeBase: null
    property var baseDraft: null
    property bool previewBase: false
    property string runtimeMode: "live"
    property var runtime: null
    property var editor: null
    property var receipt: null
    property string failure: ""
    property string readStatus: ""
    property int captured: -1
    property int callbackGetter: -1
    property int callbackBinding: -1
    property int readCompletions: 0
    property int nativeCompletions: 0
    property bool signalBeforeCallback: false
    property int pendingCount: 0
    readonly property int boundCount: runtime && runtime.ready ? runtime.values.count : -1
    readonly property bool ready: base.ready && owner.ready
    readonly property var earlyRead: owner.readProjection
    signal setupFinished()
    signal writerFinished()
    signal readFinished()
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "count"; valueType: ConfigValue.Integer; defaultValue: 1 }
            }
        }
        sources: [JsonFileSource { sourceId: "base"; prefix: ["settings"]; path: root.directory + "/base.json"; writable: true }]
    }
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "owner"; path: root.directory + "/owner.json"; writable: true }]
    }
    property Connections readSignals: Connections {
        target: root.runtime ? root.runtime.readProjection : null
        function onCompleted(result: var): void {
            ++root.nativeCompletions;
            if (result.status === "observed" && result.pending.length === 0
                && root.boundCount !== result.snapshot.state(["count"]).value)
                root.failure = "bound property not current at read signal";
        }
    }
    function options(mode: string): var {
        return {mode: mode, base: mode === "editor" ? baseScope : runtimeBase, ownerPath: ["rows"],
            ownerGeneration: owner.info(["rows"]).publicationGeneration,
            entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"};
    }
    function setup(): void {
        if (earlyRead !== owner.readProjection) {
            failure = "read projection identity changed";
            setupFinished();
            return;
        }
        baseScope = base.scope(["settings"], false);
        if (previewBase) {
            baseDraft = base.draft([["settings"]]);
            baseDraft.values.settings.count = 3;
            runtimeBase = baseDraft.previewScope(["settings"]);
        } else runtimeBase = baseScope;
        owner.openOverlay(options(runtimeMode), function(opened) {
            if (opened.status !== "opened") {
                failure = "runtime open:" + opened.error.code;
                setupFinished();
                return;
            }
            runtime = opened.editor;
            owner.openOverlay(options("editor"), function(result) {
                if (result.status !== "opened") failure = "editor open:" + result.error.code;
                else editor = result.editor;
                setupFinished();
            });
        });
    }
    function apply(value: int, edit: bool): void {
        if (previewBase) editor.unset(["count"]);
        else if (edit) editor.values.count = value;
        editor.apply(function(write) {
            receipt = write;
            if (write.status === "failed" || write.refreshState !== "current")
                failure = "writer not saved/current";
            writerFinished();
        });
    }
    function synchronize(withToken: bool): void {
        const complete = function(read) {
            readStatus = read.status;
            signalBeforeCallback = nativeCompletions === readCompletions + 1;
            ++readCompletions;
            pendingCount = read.pending.length;
            if (pendingCount !== 0 && (read.pending[0].length !== 1 || read.pending[0][0] !== "count"))
                failure = "unexpected pending input path";
            callbackGetter = runtime.values.count;
            callbackBinding = boundCount;
            if (read.status === "observed") captured = read.snapshot.state(["count"]).value;
            readFinished();
        };
        if (withToken) runtime.readProjection.synchronize(receipt.observation, complete);
        else runtime.readProjection.synchronize(complete);
    }
    function newerInput(): void { runtime.values.count = 7; }
    function closeEditor(): void { editor.detach(); }
    function rejectsForged(): bool {
        try {
            runtime.readProjection.synchronize({delivery: "999999"}, function() {});
        } catch (error) {
            return String(error).indexOf("read-observation") !== -1;
        }
        return false;
    }
}
