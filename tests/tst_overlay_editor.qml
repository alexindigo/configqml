import QtQuick
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    property string stage: "idle"
    property Connections editorSignals: Connections {
        target: root.editor
        function onCompleted(result: var): void { root.stage = "editor signal: " + result.status; }
    }
    readonly property bool ready: main.ready && base.ready
    property var runtimeView: null
    property var editor: null
    property bool busyReceipt: false
    property bool awaitingLiveConsumers: false
    readonly property int preview: editor && editor.ready ? editor.values.volume : -1
    readonly property string prototypeValue: editor && editor.ready ? Object.getOwnPropertyDescriptor(editor.values.keyMap, "__proto__").value : "absent"
    readonly property string liveOptional: runtimeView && runtimeView.ready && runtimeView.values.optional !== undefined ? runtimeView.values.optional : "absent"
    readonly property bool liveConsumersCurrent: busyReceipt && liveOptional === "new" && runtimeView && runtimeView.ready && runtimeView.values.volume === 1 && preview === 1 && main.ready && main.values.rows[0].instanceSettings === undefined
    onLiveConsumersCurrentChanged: finishLiveConsumers()
    function finishLiveConsumers(): void {
        if (awaitingLiveConsumers && liveConsumersCurrent) finished = true;
    }
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "volume"; valueType: ConfigValue.Integer; defaultValue: 1 }
                ConfigValue { key: "optional"; valueType: ConfigValue.String; optional: true }
                ConfigValue { key: "wide"; valueType: ConfigValue.UInt64; defaultValue: "18446744073709551614" }
                ConfigValue { key: "nil"; valueType: ConfigValue.String; nullable: true; defaultValue: "base" }
                ConfigValue {
                    key: "keyMap"; valueType: ConfigValue.Map
                    defaultValue: JSON.parse('{"__proto__":"base","a/b.":"opaque"}')
                    ConfigValue { valueType: ConfigValue.String }
                }
                ConfigValue {
                    key: "object"; valueType: ConfigValue.Record; defaultValue: ({a: 1, b: 2})
                    ConfigValue { key: "a"; valueType: ConfigValue.Integer }
                    ConfigValue { key: "b"; valueType: ConfigValue.Integer }
                }
                ConfigValue {
                    key: "items"; valueType: ConfigValue.List; defaultValue: [1, 2]
                    ConfigValue { valueType: ConfigValue.Integer }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "base"; layerId: "user"; prefix: ["settings"]; path: root.directory + "/base.json"; writable: true }]
    }
    property ConfigStore main: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List
                defaultValue: [{id: "x", instanceSettings: {volume: 2, opaque: {keep: false}}}, {id: "x"}]
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "main"; layerId: "user"; path: root.directory + "/main.json"; writable: true }]
    }
    function check(condition: bool, description: string): bool {
        if (!condition && failure === "") failure = description;
        return condition;
    }
    function options(mode: string): var {
        return {mode: mode, base: base.scope(["settings"], false), ownerPath: ["rows"],
                ownerGeneration: main.info(["rows"]).publicationGeneration,
                entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
                overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
                differencePolicy: "top_level_omit_equal"};
    }
    function throwsInput(action: var, description: string): bool {
        let threw = false;
        try { action(); } catch (error) { threw = true; }
        return check(threw, description);
    }
    function run(): void {
        stage = "option conversion";
        const malformed = [[], function() {}, root];
        for (let i = 0; i < malformed.length; ++i)
            if (!throwsInput(function() { main.openOverlay(malformed[i]); }, "non-record options")) return;
        let cyclic = options("editor");
        cyclic.identityValue = cyclic;
        if (!throwsInput(function() { main.openOverlay(cyclic); }, "cyclic option")) return;
        let engineBound = options("editor");
        engineBound.identityValue = root;
        if (!throwsInput(function() { main.openOverlay(engineBound); }, "engine option")) return;
        const indices = [{}, [], null, true, "0", -1, 0.5];
        for (let j = 0; j < indices.length; ++j) {
            let invalid = options("editor");
            invalid.entryPath = [{index: indices[j]}];
            if (!throwsInput(function() { main.openOverlay(invalid); }, "structural index " + j)) return;
        }
        runSemanticCase(0);
    }
    function runSemanticCase(index: int): void {
        if (index === 4) { runFlow(); return; }
        let invalid = options("editor");
        let code = "overlay-options";
        if (index === 0) invalid.mergePolicy = "source string forbidden";
        if (index === 1) {
            Object.defineProperty(invalid, "__proto__", {value: {unknown: true}, enumerable: true});
        }
        if (index === 2) { invalid.ownerPath = ["missing"]; code = "overlay-selection"; }
        if (index === 3) { invalid.identityValue = "other"; code = "overlay-identity"; }
        let returned = false;
        const id = main.openOverlay(invalid, function(result) {
            if (!check(returned && result.status === "failed" && result.error.code === code,
                       "asynchronous semantic option " + index)) return;
            runSemanticCase(index + 1);
        });
        returned = true;
        check(typeof id === "string" && id !== "", "request id");
    }
    function runFlow(): void {
        stage = "opening view";
        main.openOverlay(options("view"), function(result) {
            stage = "view opened";
            if (!check(result.status === "opened" && result.editor.ready, "view open")) return;
             runtimeView = result.editor;
             if (!throwsInput(function() { runtimeView.values.volume = 4; }, "readonly setter")) return;
             if (!throwsInput(function() { runtimeView.unset(["volume"]); }, "readonly unset")) return;
            main.openOverlay(options("editor"), function(opened) {
                stage = "editor opened";
                if (!check(opened.status === "opened", "editor open")) return;
                 editor = opened.editor;
                 if (!check(editor.values.volume === 2, "merged consumer")) return;
                 if (!throwsInput(function() { editor.values.volume = "invalid"; }, "bad typed setter")) return;
                 let cycle = []; cycle.push(cycle);
                 if (!throwsInput(function() { editor.values.items = cycle; }, "cyclic setter")) return;
                 if (!throwsInput(function() { editor.values.object = root; }, "engine setter")) return;
                 if (!check(!editor.dirty && editor.values.wide === "18446744073709551614" && editor.values.optional === undefined, "rejection no intent and wide domain")) return;
                 editor.values.volume = 7;
                 editor.values.wide = "18446744073709551615";
                 editor.values.nil = null;
                 editor.values.object = {a: 8, b: 2};
                 editor.values.items = [];
                 editor.values.keyMap = JSON.parse('{"__proto__":"kept","a/b.":"opaque"}');
                if (!check(editor.dirty && preview === 7 && main.values.rows[0].instanceSettings.volume === 2, "detached preview")) return;
                editor.apply(function(saved) {
                    stage = "apply completed";
                    if (!check(saved.status === "saved" && saved.refreshState === "current" && !editor.dirty && !editor.busy, "save/refresh receipt")) return;
                    if (!check(editor.values.wide === "18446744073709551615" && editor.values.nil === null && editor.values.object.a === 8 && editor.values.object.b === 2 && editor.values.items.length === 0 && main.values.rows[0].instanceSettings.opaque.keep === false, "owned domain consumers")) return;
                    if (!check(prototypeValue === "kept" && editor.values.keyMap["a/b."] === "opaque", "opaque map key consumers")) return;
                    editor.values.volume = 9;
                    base.setUser(["settings", "optional"], "new", function(admitted) {
                        stage = "base admitted";
                        if (!check(admitted.status === "accepted", "base input")) return;
                        editor.apply(function(conflict) {
                            stage = "conflict completed";
                            if (!check(conflict.status === "failed" && conflict.error.code === "overlay-conflict" && editor.dirty && editor.conflicted && preview === 9, "frozen editor")) return;
                            editor.discard(function(discarded) {
                                stage = "discard completed";
                                if (!check(discarded.status === "discarded" && !editor.dirty && preview === 7, "explicit discard")) return;
                                editor.resetOverrides(function(reset) {
                                    stage = "reset completed";
                                    if (!check(reset.status === "saved" && reset.refreshState === "current" && !editor.dirty, "reset receipt")) return;
                                    stage = "waiting live end consumers";
                                    awaitingLiveConsumers = true;
                                    finishLiveConsumers();
                                });
                            });
                        });
                    });
                });
                if (!throwsInput(function() { editor.values.volume = 8; }, "busy setter")) return;
                editor.discard(function(busy) {
                    if (!check(busy.status === "failed" && busy.error.code === "overlay-busy", "busy discard receipt")) return;
                    busyReceipt = true;
                });
            });
        });
    }
    function runRestart(): void {
        check(main.values.rows.length === 2 && main.values.rows[0].instanceSettings === undefined, "reset restart raw consumer");
        finished = failure === "";
    }
}
