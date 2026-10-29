import QtQml
import ConfigQml 1.0

// The driver performs foreign file operations/permission changes only. All
// acceptance/deployment decisions and assertions run in this real QML consumer.
QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    property string action: ""
    property int accepted: 0
    property int deployments: 0
    property int pluginDeployments: 0
    property var event: null
    property var pluginEvent: null
    property var deployedInput: null
    property var pluginInput: null
    property string priorRevision: ""
    readonly property var cfg: store.values
    readonly property var pluginCfg: plugin.values
    readonly property bool ready: store.ready && plugin.ready
    readonly property int observed: store.ready ? cfg.a.n : -1
    readonly property int observedOther: store.ready ? cfg.b.n : -1
    readonly property int previewA: plugin.ready ? pluginCfg.settings.a : -1
    readonly property int previewB: plugin.ready ? pluginCfg.settings.b : -1
    property ConfigStore store: ConfigStore {
        id: store
        autoSave: false
        evidenceDirectory: root.directory + "/evidence"
        observedPaths: [["a", "n"], ["b", "n"], ["a", "rows"]]
        schema: ConfigSchema {
            ConfigGroup {
                key: "a"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 }
                ConfigValue {
                    key: "rows"; valueType: ConfigValue.List
                    defaultValue: [{id:"keep"}, {id:"drop"}]
                    ConfigValue {
                        valueType: ConfigValue.Record
                        ConfigValue { key: "id"; valueType: ConfigValue.String; defaultValue: "" }
                    }
                }
            }
            ConfigGroup {
                key: "b"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 }
            }
        }
        sources: [
            JsonFileSource { sourceId: "a"; prefix: ["a"]; path: root.directory + "/live/user.json"; watchMode: JsonFileSource.Live },
            JsonFileSource { sourceId: "b"; prefix: ["b"]; path: root.directory + "/section.json"; watchMode: JsonFileSource.Live }
        ]
        recordListPolicies: [ConfigRecordListPolicy {
            policyId: "rows"; path: ["a", "rows"]; keyField: "id"; pruneUnknown: true
        }]
        onSourceAccepted: function(acceptedEvent) {
            root.event = acceptedEvent;
            root.accepted++;
            if (acceptedEvent.origin === "external") {
                root.deployments++;
                root.deployedInput = {a:root.fact(acceptedEvent, ["a","n"]).value,
                                      b:root.fact(acceptedEvent, ["b","n"]).value};
            }
        }
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    property ConfigStore plugin: ConfigStore {
        id: plugin
        autoSave: false
        observedPaths: [["settings", "a"], ["settings", "b"]]
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "a"; valueType: ConfigValue.Integer; defaultValue: 0 }
                ConfigValue { key: "b"; valueType: ConfigValue.Integer; defaultValue: 0 }
            }
        }
        sources: [
            JsonFileSource { layerId:"legacy"; sourceId:"legacy"; prefix:["settings"]; path:root.directory+"/legacy.json"; writable:false; fallbackFor:"user" },
            JsonFileSource { sourceId:"user"; prefix:["settings"]; path:root.directory+"/plugin.json" }
        ]
        onSourceAccepted: function(acceptedEvent) {
            root.pluginEvent = acceptedEvent;
            if (acceptedEvent.origin === "external") {
                root.pluginDeployments++;
                root.pluginInput = {a:root.fact(acceptedEvent,["settings","a"]).value,
                                   b:root.fact(acceptedEvent,["settings","b"]).value};
            }
        }
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function fact(acceptedEvent, path) {
        for (const selected of acceptedEvent.facts)
            if (JSON.stringify(selected.path) === JSON.stringify(path)) return selected;
        failure = "missing-selected-fact";
        return {};
    }
    function check(condition, code) {
        if (!condition && !failure.length) failure = code;
        return condition;
    }
    function run() {
        if (!check(event.origin === "startup" && event.cause === "startup" && accepted === 1
                   && fact(event,["a","n"]).value === 0 && previewA === 1 && previewB === 2,
                   "origin-startup")) return;
        cfg.a.n = 9;
        check(observed === 9 && deployments === 0, "local-invalidation-is-not-deployment");
        store.save(function(result) {
            if (root.check(result.status === "saved" && root.event.origin === "own_save"
                           && root.event.cause === "save" && root.accepted === 2
                           && root.fact(root.event,["a","n"]).value === 9 && root.deployments === 0,
                           "origin-own-save-no-deploy")) root.action = "echo";
        });
    }
    function actionDone(completedAction) {
        action = "";
        if (completedAction === "echo") {
            if (check(accepted === 2 && deployments === 0, "origin-own-live-echo-no-deploy")) {
                priorRevision = fact(event,["a","n"]).revision;
                action = "equal";
            }
        } else if (completedAction === "equal") {
            store.reload(function(result) {
                if (root.check(result.status === "reloaded" && root.deployments === 1
                               && root.deployedInput.a === 9 && root.event.origin === "external"
                               && root.fact(root.event,["a","n"]).revision !== root.priorRevision,
                               "origin-same-values-new-inode")) root.action = "cross";
            });
        } else if (completedAction === "cross") {
            store.reload(function(result) {
                if (root.check(result.status === "reloaded" && root.deployments === 2
                               && root.deployedInput.a === 5 && root.deployedInput.b === 6
                               && root.observed === 5 && root.observedOther === 6
                               && root.event.sources.length === 2,
                               "origin-coherent-cross-section")) root.action = "malformed";
            });
        } else if (completedAction === "malformed") {
            cfg.a.n = 9;
            store.reload(function(result) {
                if (root.check(result.status === "failed" && store.sourceStates[0].evidence.length > 0
                               && store.sourceStates[0].load === "parse_error"
                               && root.deployments === 2 && root.observed === 9,
                               "origin-stale-inspector-evidence")) root.action = "repair";
            });
        } else if (completedAction === "repair") {
            store.reload(function(result) {
                if (root.check(result.status === "reloaded" && root.deployments === 3
                               && root.deployedInput.a === 7 && root.observed === 9,
                               "origin-external-with-local-display")) root.action = "block";
            });
        } else if (completedAction === "block") {
            cfg.a.n = 11;
            check(observed === 11 && deployments === 3, "failed-save-local-invalidation");
            store.save(function(result) {
                if (root.check(result.status === "failed" && root.deployments === 3
                               && root.observed === 11, "origin-local-failed-save-invalidation"))
                    root.action = "unblock";
            });
        } else if (completedAction === "unblock") {
            store.reload(function(result) {
                if (!root.check(result.status === "reloaded" && root.deployments === 4
                                && root.deployedInput.a === 8 && root.observed === 11,
                                "failed-save-external-disk-choice")) return;
                store.updateRecordListPolicy("rows", {ready:true,entries:[
                    {key:"keep",available:true,retain:false,normalize:false,defaults:{}}
                ]}, function(projected) {
                    if (!root.check(projected.status === "projected" && root.event.origin === "projection"
                                    && root.event.cause === "metadata" && root.deployments === 4
                                    && root.cfg.a.rows.length === 1
                                    && root.fact(root.event,["a","rows"]).value.length === 2,
                                    "origin-projection-no-deploy")) return;
                    root.pluginCfg.settings.a = 9;
                    plugin.reload(function() { root.action = "fallback"; });
                });
            });
        } else if (completedAction === "fallback") {
            plugin.reload(function(result) {
                if (root.check(result.status === "reloaded" && root.pluginDeployments === 1
                               && root.pluginInput.a === 3 && root.pluginInput.b === 0
                               && root.previewA === 9 && root.previewB === 0,
                               "origin-fallback-disk-versus-pending")) root.action = "empty";
            });
        } else if (completedAction === "empty") {
            plugin.reload(function(result) {
                if (!root.check(result.status === "reloaded" && root.pluginDeployments === 2
                                && root.pluginInput.a === 1 && root.pluginInput.b === 2
                                && root.previewA === 9 && root.previewB === 0
                                && root.fact(root.pluginEvent,["settings","b"]).sourceId === "legacy",
                                "origin-fallback-reactivation-disk-versus-pending")) return;
                plugin.reload(function() {
                    if (root.check(root.pluginDeployments === 2, "origin-no-op-no-deploy")) root.finished = true;
                });
            });
        }
    }
}
