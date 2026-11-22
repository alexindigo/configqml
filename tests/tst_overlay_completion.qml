import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property bool readOnlySetup: false
    property var contexts: []
    property var baseScope: null
    property var page: null
    property var signalsSeen: []
    property var callbacks: []
    property var requests: []
    property string failure: ""
    property int pending: 0
    property int restoredBinding: -1
    readonly property bool ready: owner.ready && base.ready
    readonly property var settings: owner.values
    readonly property string startupError: JSON.stringify({owner: owner.error, base: base.error})
    readonly property var runtime: contexts.length ? contexts[0] : null
    readonly property bool boundEnabled: runtime && runtime.ready ? runtime.values.enabled : false
    signal setupFinished()
    signal quitFinished()
    signal effectsFinished()
    signal restoreFinished()
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "bar"
                ConfigGroup {
                    key: "widgets"
                    ConfigValue {
                        key: "left"; valueType: ConfigValue.List; defaultValue: []
                        ConfigValue {
                            valueType: ConfigValue.Record
                            ConfigValue { key: "id"; valueType: ConfigValue.String }
                        }
                    }
                }
            }
            ConfigGroup {
                key: "idle"
                ConfigValue { key: "customCommands"; valueType: ConfigValue.String; defaultValue: "[]" }
            }
            ConfigGroup {
                key: "hooks"
                ConfigValue { key: "screenLock"; valueType: ConfigValue.String; defaultValue: "" }
                ConfigValue { key: "screenUnlock"; valueType: ConfigValue.String; defaultValue: "" }
            }
        }
        sources: [
            JsonFileSource { layerId: "legacy"; sourceId: "legacy"; path: root.directory + "/settings.json"; writable: false },
            JsonFileSource { sourceId: "bar"; prefix: ["bar"]; path: root.directory + "/bar.json"; writable: true },
            JsonFileSource { sourceId: "idle"; prefix: ["idle"]; path: root.directory + "/idle.json"; writable: true },
            JsonFileSource { sourceId: "hooks"; prefix: ["hooks"]; path: root.directory + "/hooks.json"; writable: true }
        ]
    }
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "enabled"; valueType: ConfigValue.Boolean; defaultValue: true }
                ConfigValue { key: "idleSeconds"; valueType: ConfigValue.Integer; defaultValue: 300 }
            }
        }
    }
    property Connections completions: Connections {
        target: root.owner
        function onCompleted(result: var): void {
            root.signalsSeen.push(String(result.requestId));
        }
    }
    function check(result: var): void {
        if (result.status === "failed") failure = result.operation + ":" + result.error.code;
    }
    function options(mode: string, index: int): var {
        return {mode: mode, base: baseScope, ownerPath: ["bar", "widgets", "left"],
            ownerGeneration: owner.info(["bar", "widgets", "left"]).publicationGeneration,
            entryPath: [{index: index}], identityPath: [{key: "id"}], identityValue: "x",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"};
    }
    function setup(): void {
        baseScope = base.scope(["settings"], false);
        openNext(0);
    }
    function openNext(index: int): void {
        if (index === 3 && readOnlySetup) { setupFinished(); return; }
        owner.openOverlay(options(index < 3 ? "live" : "editor", index < 3 ? index : 0), function(result) {
            check(result);
            if (result.status !== "opened") { setupFinished(); return; }
            if (index < 3) {
                contexts = contexts.concat([result.editor]);
                openNext(index + 1);
            } else {
                page = result.editor;
                page.values.idleSeconds = 450;
                page.apply(function(write) {
                    check(write);
                    page.detach();
                    page = null;
                    setupFinished();
                });
            }
        });
    }
    function saveSelected(restoring: bool): void {
        pending++;
        requests.push(String(owner.saveSelected([["bar", "widgets", "left"]], function(result) {
            check(result);
            callbacks.push(String(result.requestId));
            if (signalsSeen.indexOf(String(result.requestId)) < 0) failure = "callback before signal";
            if (restoring) restoredBinding = boundEnabled ? 1 : 0;
            pending--;
            if (restoring) restoreFinished(); else quitFinished();
        })));
    }
    function quit(): void {
        runtime.values.enabled = false;
        saveSelected(false);
    }
    function effects(enabled: bool): void {
        settings.idle.customCommands = enabled ? "[1]" : "[]";
        settings.hooks.screenLock = enabled ? "kill" : "";
        settings.hooks.screenUnlock = enabled ? "kill" : "";
        requests.push(String(owner.save(function(result) {
            check(result);
            callbacks.push(String(result.requestId));
            if (signalsSeen.indexOf(String(result.requestId)) < 0) failure = "callback before signal";
            effectsFinished();
        })));
    }
    function restore(): void {
        runtime.values.enabled = true;
        saveSelected(true);
        effects(true);
    }
}
