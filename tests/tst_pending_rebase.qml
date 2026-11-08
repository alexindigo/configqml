import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property var cfg: store.values
    readonly property int observedA: store.ready ? cfg.a : -1
    readonly property int observedB: store.ready ? cfg.b : -1
    property int warnings: 0
    property var origins: []
    property ConfigStore store: ConfigStore {
        autoSave: false
        evidenceDirectory: root.directory + "/evidence"
        observedPaths: [["b"]]
        schema: ConfigSchema {
            ConfigValue { key: "a"; valueType: ConfigValue.Integer; defaultValue: 0 }
            ConfigValue { key: "b"; valueType: ConfigValue.Integer; defaultValue: 0 }
        }
        sources: [JsonFileSource {
            sourceId: "user"; path: root.directory + "/user.json"
            conflictPolicy: JsonFileSource.StrictPendingRebase
        }]
        onSourceAccepted: function(event) {
            root.origins.push(event.origin);
            if (event.origin === "external" && (event.facts[0].value !== 4 || root.observedB !== 9))
                root.failure = "preflight-fact-or-pending-consumer";
        }
        onSourceWarning: function(warning) {
            if (warning.code !== "foreign-value-overridden" || warning.phase !== "rebase"
                    || warning.path.join(".") !== "b" || warning.evidence.length === 0)
                root.failure = "rebase-warning";
            root.warnings++;
        }
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        if (JsonFileSource.StrictPendingRebase !== 2 || observedA !== 1 || observedB !== 1) {
            failure = "rebase-initial";
            return;
        }
        cfg.b = 9;
    }
    function foreignWritten() {
        store.save(function(result) {
            const fact = result.publications[0];
            if (result.status !== "saved" || observedA !== 7 || observedB !== 9
                    || warnings !== 1 || !fact.rebasedOnto
                    || fact.overriddenPaths.length !== 1 || fact.overriddenPaths[0].join(".") !== "b"
                    || origins.join(",") !== "startup,external,own_save")
                failure = "rebase-receipt-or-consumer";
            else
                finished = true;
        });
    }
    function runRestart() {
        if (observedA !== 7 || observedB !== 9 || warnings !== 0)
            failure = "rebase-restart";
        else
            finished = true;
    }
}
