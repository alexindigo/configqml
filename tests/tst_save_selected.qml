import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    property string stage: ""
    property string token: ""
    property string prefix: ""
    property var seen: ({})
    readonly property bool ready: store.ready
    readonly property var values: store.values
    property ConfigStore owner: ConfigStore {
        id: store
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "ui"
                ConfigValue { key: "a"; valueType: ConfigValue.Integer; defaultValue: 0 }
                ConfigValue { key: "b"; valueType: ConfigValue.Integer; defaultValue: 0 }
            }
            ConfigGroup {
                key: "binding"
                ConfigValue { key: "key"; valueType: ConfigValue.Integer; defaultValue: 0 }
            }
            ConfigValue { key: "other"; valueType: ConfigValue.Integer; defaultValue: 0 }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json" }]
        onCompleted: function(result) { root.seen[result.requestId] = true; }
    }
    function check(result, status, code) {
        if (!seen[result.requestId] || result.status !== status
                || (code && result.error.code !== code)) {
            failure = "receipt-" + result.operation + "-" + result.status + "-" + result.error.code;
            return false;
        }
        return true;
    }
    function selected(paths, status, continuation, code) {
        let returned = false;
        const id = store.saveSelected(paths, function(result) {
            if (!returned || result.requestId !== id || result.operation !== "saveSelected"
                    || !check(result, status, code)) {
                failure = failure || "selected-order";
                return;
            }
            continuation(result);
        });
        returned = true;
    }
    function run() {
        if (typeof store.saveSelected !== "function") {
            failure = "missing-saveSelected";
            return;
        }
        store.retainGroup({requestKey: "head", paths: [["binding"]], edits: [
            {path: ["binding", "key"], present: true, value: 7}
        ], dependencies: []}, function(result) {
            if (!check(result, "retained")) return;
            token = result.groupToken;
            prefix = result.capturePrefix;
            values.ui.a = 1;
            values.ui.b = 2;
            values.other = 9;
            selected([["ui", "a"]], "saved", function(receipt) {
                if (receipt.coverage.length !== 1 || receipt.unresolvedGroups.length !== 0
                        || receipt.excludedGroups[0] !== token) {
                    failure = "selected-coverage";
                    return;
                }
                stage = "leaf";
            });
        });
    }
    function advance() {
        if (stage === "leaf") {
            selected([["ui", "a"]], "nothing_to_save", function(receipt) {
                if (receipt.publications.length !== 0 || receipt.coverage.length !== 0
                        || Object.keys(receipt.coveredRevisions).length !== 0) {
                    failure = "no-op-coverage";
                    return;
                }
                selected([], "failed", function() {
                    selected([["missing"]], "failed", function() {
                        selected([[42]], "failed", function() {
                            selected([["ui"]], "saved", function() { stage = "namespace"; });
                        }, "save-selection");
                    }, "save-selection");
                }, "save-selection");
            });
        } else if (stage === "namespace") {
            values.ui.a = 3;
            selected([["ui", "a"], ["other"]], "saved", function() { stage = "disjoint"; });
        } else if (stage === "disjoint") {
            store.reset(["ui", "a"], "user");
            selected([["ui", "a"]], "saved", function() { stage = "reset"; });
        } else if (stage === "reset") {
            values.ui.b = 4;
            selected([["ui"], ["binding"]], "failed", function(receipt) {
                if (receipt.publications.length !== 0 || receipt.unresolvedGroups[0] !== token
                        || store.groupInfo(token).state !== "retained") {
                    failure = "overlap-effects";
                    return;
                }
                stage = "overlap";
            }, "unresolved-group");
        } else if (stage === "overlap") {
            const scope = store.scope(["ui"]);
            scope.saveSelected([["b"]], function(receipt) {
                if (!check(receipt, "saved")) return;
                store.save(function(full) {
                    if (!check(full, "failed", "unresolved-group")) return;
                    store.saveGroup(token, function(saved) {
                        if (!check(saved, "saved") || saved.groupToken !== token
                                || saved.capturePrefix !== prefix || saved.attemptNumber !== "1") {
                            failure = failure || "retained-identity";
                            return;
                        }
                        stage = "final";
                        finished = true;
                    });
                });
            });
        }
    }
    function runRestart() {
        if (values.ui.a !== 0 || values.ui.b !== 4
                || values.other !== 9 || values.binding.key !== 7)
            failure = "selected-cold-values";
        else
            finished = true;
    }
}
