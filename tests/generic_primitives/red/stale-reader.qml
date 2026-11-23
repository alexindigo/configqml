import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property var baseScope: null
    property var runtime: null
    property var editor: null
    property string failure: ""
    property string writeStatus: ""
    property string refreshState: ""
    property int writerAtCompletion: -1
    property int readerAtCompletion: -1
    property int bindingAtCompletion: -1
    readonly property int boundCount: runtime && runtime.ready ? runtime.values.count : -1
    readonly property bool ready: base.ready && owner.ready
    signal setupFinished()
    signal writerFinished()
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
    function options(mode: string): var {
        return {mode: mode, base: baseScope, ownerPath: ["rows"],
            ownerGeneration: owner.info(["rows"]).publicationGeneration,
            entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"};
    }
    function setup(): void {
        baseScope = base.scope(["settings"], false);
        owner.openOverlay(options("live"), function(opened) {
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
    function apply(): void {
        editor.values.count = 3;
        editor.apply(function(write) {
            writeStatus = write.status;
            refreshState = write.refreshState;
            writerAtCompletion = editor.values.count;
            readerAtCompletion = runtime.values.count;
            bindingAtCompletion = boundCount;
            writerFinished();
        });
    }
}
