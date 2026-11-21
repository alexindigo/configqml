import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    required property int kind
    required property var initial
    property string mode: ""
    property string failure: ""
    property bool done: false
    property var scope: null
    property var draft: null
    property var subject: null
    property var observed: subject ? subject.flag : undefined
    readonly property var sharedValues: shared.values
    property int changes: 0
    onObservedChanged: ++changes
    property var defaultObserved: subject ? subject.defaulted : undefined
    property var maskedObserved: subject ? subject.masked : undefined
    property int defaultChanges: 0
    property int maskedChanges: 0
    onDefaultObservedChanged: ++defaultChanges
    onMaskedObservedChanged: ++maskedChanges
    readonly property bool ready: shared.ready && owner.ready
    property ConfigStore shared: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "flag"; valueType: root.kind; optional: true; nullable: true; open: root.kind === ConfigValue.Record }
                ConfigValue { key: "defaulted"; valueType: ConfigValue.Boolean; defaultValue: false }
                ConfigValue { key: "masked"; valueType: ConfigValue.Boolean; optional: true }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; prefix: ["settings"]; path: root.directory + "/user.json"; writable: true; watchMode: JsonFileSource.Live },
            MemorySource { layerId: "runtime"; sourceId: "runtime"; prefix: ["settings"]; kind: MemorySource.Runtime; writable: false; data: ({masked: true}) }]
    }
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue { valueType: ConfigValue.Record; open: true
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "owner"; path: root.directory + "/owner.json"; writable: true }]
    }
    property var overlay: null
    function complete(result): void {
        if (result.status === "failed") failure = result.error.code;
        done = true;
    }
    function setup(selected: string): void {
        mode = selected;
        scope = shared.scope(["settings"]);
        if (mode === "draft" || mode === "preview") {
            draft = scope.draft([[]]);
            subject = mode === "draft" ? draft.values : draft.preview;
        } else if (mode === "overlay-editor" || mode === "overlay-live" || mode === "inherited-view" || mode === "inherited-live") {
            owner.openOverlay({mode: mode === "overlay-live" || mode === "inherited-live" ? "live" : mode === "inherited-view" ? "view" : "editor", base: scope,
                ownerPath: ["rows"], ownerGeneration: owner.info(["rows"]).publicationGeneration,
                entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
                overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
                differencePolicy: "top_level_omit_equal"}, function(result) {
                    if (result.status !== "opened") { failure = "open"; done = true; return; }
                    overlay = result.editor;
                    subject = overlay.values;
                    done = true;
                });
            return;
        } else if (mode === "early") subject = sharedValues.settings;
        else subject = scope.values;
        done = true;
    }
    function removeValue(): void {
        done = false;
        if (mode === "draft" || mode === "preview") draft.stageReset([["flag"]], complete);
        else if (mode === "apply") {
            draft = scope.draft([[]]);
            draft.stageReset([["flag"]], function(result) {
                if (result.status === "failed") complete(result);
                else draft.apply(complete);
            });
        } else if (overlay && mode !== "inherited-view" && mode !== "inherited-live") {
            if (!overlay.unset(["flag"])) failure = "unset";
            if (mode === "overlay-live") overlay.flush(complete);
            else done = true;
        } else scope.reset(["flag"], "user", complete);
    }
    function restoreValue(): void {
        done = false;
        if (mode === "draft" || mode === "preview") { draft.discard(); done = true; }
        else if (overlay && mode !== "inherited-view" && mode !== "inherited-live") {
            overlay.values.flag = initial;
            if (mode === "overlay-live") overlay.flush(complete);
            else done = true;
        } else { scope.values.flag = initial; scope.flush(complete); }
    }
}
