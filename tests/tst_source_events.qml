import QtQml
import ConfigQml 1.0

// The driver performs foreign file operations/file-type obstructions only. All
// acceptance/deployment decisions and assertions run in this real QML consumer.
QtObject {
    id: root
    required property string directory
    property int crossSchedule: 0
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
    property bool crossing: false
    property var crossEvents: []
    readonly property int crossAccepted: crossEvents.length
    property int crossDeployments: 0
    readonly property var cfg: store.values
    readonly property var pluginCfg: plugin.values
    readonly property bool ready: store.ready && plugin.ready
    readonly property int observed: store.ready ? cfg.a.n : -1
    readonly property int observedOther: store.ready ? cfg.b.n : -1
    readonly property int previewA: plugin.ready ? pluginCfg.settings.a : -1
    readonly property int previewB: plugin.ready ? pluginCfg.settings.b : -1
    property ConfigStore store: ConfigStore {
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
            if (!root.checkEvent(acceptedEvent, root.event, ["a", "b"],
                                 [["a", "n"], ["b", "n"], ["a", "rows"]])) return;
            root.event = acceptedEvent;
            root.accepted++;
            if (acceptedEvent.origin === "external") {
                root.deployments++;
                root.deployedInput = {a:root.fact(acceptedEvent, ["a","n"]).value,
                                      b:root.fact(acceptedEvent, ["b","n"]).value};
                if (root.crossing) {
                    const input = root.deployedInput;
                    const prior = root.crossEvents;
                    if (!root.check(input.a === 5 && (input.b === 0 || input.b === 6)
                                    && root.observed === input.a && root.observedOther === input.b
                                    && (input.b !== 0 || acceptedEvent.cause === "watch")
                                    && (prior.length === 0 || (prior.length === 1
                                        && prior[0].b === 0 && input.b === 6)),
                                    "origin-cross-event-coherence")) return;
                    root.crossEvents = prior.concat([{a:input.a, b:input.b}]);
                }
            }
        }
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    property ConfigStore plugin: ConfigStore {
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
            if (!root.checkEvent(acceptedEvent, root.pluginEvent, ["legacy", "user"],
                                 [["settings", "a"], ["settings", "b"]])) return;
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
    function checkEvent(current, previous, sourceIds, paths) {
        const causes = {startup:"startup", own_save:"save", projection:"metadata"};
        if (!check((current.origin === "external"
                    ? current.cause === "watch" || current.cause === "reload"
                    : causes[current.origin] === current.cause)
                   && Number(current.eventId) > (previous ? Number(previous.eventId) : 0)
                   && Number(current.publicationGeneration)
                      > (previous ? Number(previous.publicationGeneration) : 0),
                   "origin-event-identity-and-cause")) return false;
        if (!check(current.sources.length === sourceIds.length && current.facts.length === paths.length,
                   "origin-complete-event-witness")) return false;
        const revisions = {};
        for (let i = 0; i < sourceIds.length; ++i) {
            if (!check(current.sources[i].sourceId === sourceIds[i], "origin-source-order")) return false;
            revisions[sourceIds[i]] = current.sources[i].revision;
        }
        const changed = [];
        for (const path of paths) {
            const selected = fact(current, path);
            if (!check(selected.present && (selected.sourceId === "defaults"
                       || (sourceIds.indexOf(selected.sourceId) !== -1
                           && selected.revision === revisions[selected.sourceId])),
                       "origin-fact-source-revision")) return false;
            if (!previous || JSON.stringify(selected) !== JSON.stringify(fact(previous, path)))
                changed.push(JSON.stringify(path));
        }
        // Projection can invalidate a path without changing its disk fact.
        if (current.origin !== "projection"
                && !check(JSON.stringify(changed.sort())
                          === JSON.stringify(current.changedPaths.map(path => JSON.stringify(path)).sort()),
                          "origin-exact-changed-facts")) return false;
        return true;
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
                                "origin-same-values-new-inode")) {
                    root.crossing = true;
                    root.action = "cross";
                }
            });
        } else if (completedAction === "cross") {
            store.reload(function(result) {
                // Live watches may accept A before B, or both in one generation.
                // Neither schedule may invent duplicates, reorder or mix facts.
                if (root.check(result.status === "reloaded" && root.crossAccepted >= 1
                               && root.crossAccepted <= 2 && root.deployments === 1 + root.crossAccepted
                               && (root.crossSchedule >= 0 || root.crossAccepted === 2)
                               && root.crossEvents[root.crossAccepted - 1].b === 6
                                && root.deployedInput.a === 5 && root.deployedInput.b === 6
                               && root.observed === 5 && root.observedOther === 6
                               && root.event.sources.length === 2,
                                "origin-coherent-cross-section")) {
                    root.crossing = false;
                    root.crossDeployments = root.deployments;
                    root.action = "malformed";
                }
            });
        } else if (completedAction === "malformed") {
            cfg.a.n = 9;
            store.reload(function(result) {
                if (root.check(result.status === "failed" && store.sourceStates[0].evidence.length > 0
                               && store.sourceStates[0].load === "parse_error"
                                && root.deployments === root.crossDeployments && root.observed === 9,
                               "origin-stale-inspector-evidence")) root.action = "repair";
            });
        } else if (completedAction === "repair") {
            store.reload(function(result) {
                if (root.check(result.status === "reloaded" && root.deployments === root.crossDeployments + 1
                               && root.deployedInput.a === 7 && root.observed === 9,
                               "origin-external-with-local-display")) root.action = "block";
            });
        } else if (completedAction === "block") {
            cfg.a.n = 11;
            check(observed === 11 && deployments === crossDeployments + 1, "failed-save-local-invalidation");
            store.save(function(result) {
                if (root.check(result.status === "failed" && result.error.code === "lock-failed"
                                && root.deployments === root.crossDeployments + 1
                               && root.observed === 11, "origin-local-failed-save-invalidation"))
                    root.action = "unblock";
            });
        } else if (completedAction === "unblock") {
            store.reload(function(result) {
                if (!root.check(result.status === "reloaded" && root.deployments === root.crossDeployments + 2
                                && root.deployedInput.a === 8 && root.observed === 11,
                                "failed-save-external-disk-choice")) return;
                store.updateRecordListPolicy("rows", {ready:true,entries:[
                    {key:"keep",available:true,retain:false,normalize:false,defaults:{}}
                ]}, function(projected) {
                    if (!root.check(projected.status === "projected" && root.event.origin === "projection"
                                     && root.event.cause === "metadata" && root.deployments === root.crossDeployments + 2
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
