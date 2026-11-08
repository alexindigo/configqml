import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string filePath
    required property string ancestorId
    readonly property bool ready: store.ready
    readonly property string error: store.error
    readonly property var cfg: store.values
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "screens"; valueType: ConfigValue.List
                defaultValue: [
                    {name:"DP-1", opaque:true, widgets:[{id:"hidden", opaque:true}, {id:"clock", deprecated:true}]},
                    {name:"prune", position:"top"}
                ]
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "name"; valueType: ConfigValue.String }
                    ConfigValue { key: "position"; valueType: ConfigValue.String; optional: true }
                    ConfigValue {
                        key: "widgets"; valueType: ConfigValue.List; optional: true
                        ConfigValue {
                            valueType: ConfigValue.Record
                            ConfigValue { key: "id"; valueType: ConfigValue.String }
                            ConfigValue { key: "compact"; valueType: ConfigValue.Boolean; optional: true }
                        }
                    }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.filePath }]
        recordListPolicies: [
            ConfigRecordListPolicy { policyId: root.ancestorId; path: ["screens"]; keyField: "name" },
            ConfigRecordListPolicy {
                policyId: "m-widgets"; path: ["screens"]; keyField: "id"
                listPath: [{each:true}, {key:"widgets"}]
                pruneUnknown: true; normalizeFields: true
            }
        ]
    }
    function metadata(done) {
        root.store.updateRecordListPolicy("m-widgets", {ready:true, entries:[
            {key:"clock", available:true, retain:false, normalize:true, defaults:{compact:false}}
        ]}, function(result) { done(result.status === "projected" ? "" : "identity-metadata"); });
    }
    function command(edits, done) {
        root.store.editRecordList(root.ancestorId, {generation:root.store.info(["screens"]).publicationGeneration,
            listAddress:[], edits:edits}, function(result) {
            done(result.status === "admitted" ? "" : "identity-edit:" + result.error.code);
        });
    }
    function view(names, keys) {
        return root.cfg.screens.map(row => row.name).join("|") === names
            && Object.keys(root.cfg.screens[0]).sort().join("|") === keys
            && root.cfg.screens[0].widgets.length === 1
            && root.cfg.screens[0].widgets[0].id === "clock"
            && root.cfg.screens[0].widgets[0].compact === false;
    }
    function run(done) {
        root.metadata(function(error) {
            if (error || !root.view("DP-1|prune", "name|opaque|widgets")) { done(error || "identity-initial-view"); return; }
            root.command([{op:"set", index:0, field:"position", value:"left"}], function(setError) {
                if (setError || !root.view("DP-1|prune", "name|opaque|position|widgets")) { done(setError || "identity-set-view"); return; }
                root.command([{op:"unset", index:1, field:"position"}], function(unsetError) {
                    if (unsetError || Object.keys(root.cfg.screens[1]).join("|") !== "name") { done(unsetError || "identity-prune-key-set"); return; }
                    root.command([{op:"remove", index:1}], function(removeError) {
                        if (removeError) { done(removeError); return; }
                        root.command([{op:"insert", index:root.cfg.screens.length, value:{name:"new", position:"right"}}], function(insertError) {
                            if (insertError || !root.view("DP-1|new", "name|opaque|position|widgets")) { done(insertError || "identity-insert-view"); return; }
                            root.store.save(function(saved) { done(saved.status === "saved" ? "" : "identity-save"); });
                        });
                    });
                });
            });
        });
    }
    function runRestart(done) {
        root.metadata(function(error) {
            done(error || (root.view("DP-1|new", "name|opaque|position|widgets")
                && root.cfg.screens[0].position === "left" && root.cfg.screens[0].opaque === true
                ? "" : "identity-restart"));
        });
    }
}
