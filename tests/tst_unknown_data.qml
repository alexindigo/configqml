import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property var cfg: store.values
    property int warningCount: 0
    property ConfigStore store: ConfigStore {
        autoSave: false
        evidenceDirectory: root.directory + "/evidence"
        schema: ConfigSchema {
            ConfigValue {
                key: "record"; valueType: ConfigValue.Record; defaultValue: ({id:"default"})
                ConfigValue { key: "id"; valueType: ConfigValue.String }
            }
            ConfigValue { key: "open"; valueType: ConfigValue.Record; open: true; defaultValue: ({}) }
        }
        sources: [JsonFileSource {
            sourceId: "user"; path: root.directory + "/user.json"
            unknownData: JsonFileSource.Strict
        }]
        onSourceWarning: function(warning) {
            if (warning.phase !== "unknown-data" || warning.code !== "unknown-data-removed"
                    || warning.sourceId !== "user" || warning.file !== root.directory + "/user.json")
                root.failure = "unknown-warning-shape";
            root.warningCount++;
        }
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        if (root.warningCount !== 2 || root.cfg.record.extra !== undefined || root.cfg.open.opaque !== true) {
            root.failure = "unknown-load"; return;
        }
        let refused = false;
        try { root.store.setUser(["record"], {id:"bad", extra:7}); }
        catch (error) { refused = String(error).indexOf("undeclared-member") !== -1; }
        if (!refused) { root.failure = "unknown-setter"; return; }
        root.cfg.open = {authored: {opaque:true}};
        root.cfg.record = {id:"final"};
        root.store.save(function(saved) {
            if (saved.status !== "saved" || saved.evidence.length === 0)
                root.failure = "unknown-save";
            else root.finished = true;
        });
    }
    function runRestart() {
        if (root.warningCount !== 0 || root.cfg.record.id !== "final" || !root.cfg.open.authored.opaque)
            root.failure = "unknown-restart";
        else root.finished = true;
    }
}
