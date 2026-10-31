import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property var cfg: configuration.values
    readonly property bool ready: configuration.ready
    readonly property int observed: ready ? cfg.count : -1
    property int effectCalls: 0
    property int warnings: 0
    property ConfigStore store: ConfigStore {
        id: configuration
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "oldCount"; valueType: ConfigValue.Integer; optional: true }
            ConfigValue { key: "count"; valueType: ConfigValue.Integer; optional: true }
        }
        sources: [JsonFileSource {
            layerId: "legacy"; sourceId: "legacy"; path: root.directory + "/legacy.json"
            writable: false
        }]
        projections: [ConfigProjection {
            projectionId: "literal"; sourceIds: ["legacy"]
            transform: `function(input) {
                var has = Object.prototype.hasOwnProperty;
                var v = input.values;
                var r = input.compatibilityRaw;
                if (has.call(input.raw, "oldCount") && !has.call(input.raw, "count")) {
                    v.count = input.raw.oldCount;
                    delete v.oldCount;
                }
                if (has.call(r, "oldCount")) {
                    r.count = r.oldCount;
                    delete r.oldCount;
                }
                if (input.raw.wide.decimal !== "18446744073709551615")
                    throw new Error("atom");
                return {values:v, compatibilityRaw:r,
                        effects:[{effectId:"effect",version:1,action:"action",arguments:{}}],
                        diagnostics:[{code:"authored-note",path:["count"],severity:"warning"}]};
            }`
        }]
        onProjectionEffects: function(event) {
            root.effectCalls++;
            if (event.effects.length !== 1 || root.cfg.count !== 7)
                root.failure = "effect-before-cache";
        }
        onSourceWarning: function(warning) {
            root.warnings++;
            if (warning.code !== "authored-note" || warning.phase !== "projection-warning"
                    || warning.sourceId !== "legacy" || root.cfg.count !== 7)
                root.failure = "warning-before-cache";
        }
        onSourceError: function(error) { root.failure = "unexpected-source-error:" + error.code; }
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        if (observed !== 7 || configuration.info(["oldCount"]).present || effectCalls !== 1 || warnings < 1) {
            failure = "projection-literal-protocol";
            return;
        }
        configuration.reload(function(result) {
            if (result.status !== "reloaded" || root.observed !== 7 || root.effectCalls !== 1)
                root.failure = "projection-effects-once-and-ordered";
            else
                root.finished = true;
        });
    }
    function runRestart() { run(); }
}
