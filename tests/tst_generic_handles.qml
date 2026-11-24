import QtQml
import ConfigQml 1.0

// The five addendum-02 sketches, run cold by a native driver against real files.
QtObject {
    id: root
    required property string directory
    property string failure: ""
    property var runtime: null
    property var editor: null
    property var edit: null
    property var autoTileReply: null
    property int captured: -1
    property int boundAtFinish: -1
    property bool finishedEarly: false
    property int scaleNotifications: 0
    property int labelNotifications: 0
    property int instanceDisplayed: -1
    property bool instanceInherited: false
    readonly property bool ready: base.ready && owner.ready
    // Sketch 1: automatic binding, no per-field registration.
    readonly property var baseRoot: ConfigPrimitives.forStore(base)?.root ?? null
    readonly property var runtimeHandle: ConfigPrimitives.forOverlay(runtime)?.root ?? null
    readonly property var overlayHandle: ConfigPrimitives.forOverlay(editor)?.root ?? null
    readonly property var general: baseRoot ? baseRoot.scopes.general : null
    readonly property real scale: general ? general.fields.scaleRatio.value : -1
    readonly property bool labelPresent: general ? general.fields.label.present : false
    readonly property real editedScale: edit ? edit.fields.scaleRatio.value : -1
    readonly property int runtimeCount: runtimeHandle ? runtimeHandle.fields.count.value : -1
    // Sketch 5: sparse instance override with inherited display.
    readonly property var field: overlayHandle ? overlayHandle.fields.count : null
    readonly property var displayed: field ? field.value : undefined
    readonly property bool inherited: field ? (field.state.pending === "remove"
        || (!field.state.targetPresent && field.state.pending === "none")) : false
    readonly property var configurationPaths: [["scaleRatio"], ["label"]]
    signal finished(string name)
    onScaleChanged: ++scaleNotifications
    onLabelPresentChanged: ++labelNotifications
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "count"; valueType: ConfigValue.Integer; defaultValue: 1 }
            }
            ConfigGroup {
                key: "general"
                ConfigValue { key: "scaleRatio"; valueType: ConfigValue.Real; defaultValue: 1.0 }
                ConfigValue { key: "label"; valueType: ConfigValue.String; optional: true }
                ConfigValue { key: "notes"; valueType: ConfigValue.String; defaultValue: "" }
            }
            ConfigGroup {
                key: "bar"
                ConfigValue { key: "position"; valueType: ConfigValue.String; defaultValue: "bottom" }
            }
        }
        sources: [
            JsonFileSource { sourceId: "settings"; prefix: ["settings"]; path: root.directory + "/settings.json"; writable: true },
            JsonFileSource { sourceId: "general"; prefix: ["general"]; path: root.directory + "/general.json"; writable: true },
            JsonFileSource { sourceId: "bar"; prefix: ["bar"]; path: root.directory + "/bar.json"; writable: true }
        ]
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
    function fail(text: string): void {
        if (!failure) failure = text;
    }
    function done(reply: var, name: string): void {
        reply.finished.connect(function () {
            if (reply.isError) fail(name + ":" + reply.error.phase + ":" + reply.error.code);
            finished(name);
        });
    }
    function overlayOptions(mode: string): var {
        return {mode: mode, base: base.scope(["settings"], false), ownerPath: ["rows"],
            ownerGeneration: owner.info(["rows"]).publicationGeneration,
            entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"};
    }
    function openOverlays(): void {
        owner.openOverlay(overlayOptions("live"), function (opened) {
            if (opened.status !== "opened") { fail("runtime open"); finished("overlays"); return; }
            runtime = opened.editor;
            owner.openOverlay(overlayOptions("editor"), function (result) {
                if (result.status !== "opened") fail("editor open");
                else {
                    editor = result.editor;
                    const view = ConfigPrimitives.forOverlay(editor);
                    if (editor.root !== undefined || editor.readProjection !== undefined
                            || !view || view !== ConfigPrimitives.forOverlay(editor)
                            || !view.root || !view.readProjection)
                        fail("overlay lexical surface or adapter identity");
                }
                finished("overlays");
            });
        });
    }
    // Sketch 1: edit opening constructs no field intent.
    function openEditor(): void {
        const reply = general.edit();
        reply.finished.connect(function () {
            if (reply.isError) fail("edit:" + reply.error.code);
            else edit = reply.value.handle;
            if (edit && edit.fields.scaleRatio.state.pending !== "none") fail("opening staged intent");
            finished("openEditor");
        });
    }
    // Sketch 2: cancellable whole-configuration Reset, undisplayed leaves included.
    function resetConfiguration(): void { done(edit.select(configurationPaths).remove(), "reset"); }
    function cancelReset(): void { done(edit.select(configurationPaths).discard(), "cancel"); }
    function setScale(value: real): void { done(edit.fields.scaleRatio.set(value), "setScale"); }
    function applyEdit(): void { done(edit.apply({observe: [general]}), "applyEdit"); }
    // Sketch 3: runtime-fresh Apply; consume the acknowledged snapshot only.
    function applyAutoTile(value: int): void {
        const set = overlayHandle.fields.count.set(value);
        set.finished.connect(function () {
            if (set.isError) { fail("autotile set:" + set.error.code); finished("autoTile"); return; }
            const reply = overlayHandle.apply({observe: [runtimeHandle]});
            autoTileReply = reply;
            reply.finished.connect(function () {
                if (reply.isError) { fail("autotile:" + reply.error.phase + ":" + reply.error.code); finished("autoTile"); return; }
                boundAtFinish = runtimeCount;
                const state = reply.value.observations[0].snapshot.state(["count"]);
                if (!state.valid) { fail("autotile snapshot not observed"); finished("autoTile"); return; }
                captured = state.value;
                if (!reply.value.write) fail("autotile write facts missing");
                finished("autoTile");
            });
            finishedEarly = reply.isFinished;
        });
    }
    // Sketch 4: scoped save excludes pending work in a sibling.
    function saveBarOnly(): void {
        baseRoot.scopes.general.fields.scaleRatio.set(9);
        baseRoot.scopes.bar.fields.position.set("left");
        done(baseRoot.scopes.bar.apply(), "saveBar");
    }
    // Sketch 5: sparse override; removal is not a default set.
    function choose(value: int): void { done(field.set(value), "choose"); }
    function inheritAgain(): void { done(field.unset(), "inherit"); }
    function applyInstance(): void {
        const reply = overlayHandle.apply({observe: [runtimeHandle]});
        reply.finished.connect(function () {
            if (reply.isError) fail("instance:" + reply.error.phase + ":" + reply.error.code);
            instanceDisplayed = displayed === undefined ? -1 : displayed;
            instanceInherited = inherited;
            finished("instance");
        });
    }
    function forgedToken(): void {
        const reply = runtimeHandle.observe({delivery: "999"});
        reply.finished.connect(function () {
            if (!reply.isError || reply.error.code !== "read-observation") fail("forged token accepted");
            finished("forged");
        });
    }
    function readOnlyRefusal(): void {
        const reply = ConfigPrimitives.forStore(base.scope(["bar"], false)).root.fields.position.set("x");
        reply.finished.connect(function () {
            if (!reply.isError || reply.error.code !== "unsupported-operation") fail("read-only write");
            finished("readOnly");
        });
    }
}
