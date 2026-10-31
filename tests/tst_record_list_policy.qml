import QtQml
import QtQuick
import ConfigQml 1.0

Item {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property int observedCount: store.ready ? store.values.rows.length : -1
    readonly property bool observedCompact: store.ready && store.values.rows.length === 1 ? store.values.rows[0].compact : true
    Repeater {
        id: rows
        model: root.ready ? store.listModel(["rows"]) : null
        delegate: Text {
            required property var modelData
            text: modelData.get("id").value
        }
    }
    property ConfigStore store: ConfigStore {
        id: store
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List
                defaultValue: [{id:"hidden"}, {id:"clock", deprecated:3}]
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                    ConfigValue { key: "compact"; valueType: ConfigValue.Boolean; optional: true }
                    ConfigValue { key: "wide"; valueType: ConfigValue.UInt64; optional: true }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json" }]
        recordListPolicies: [ConfigRecordListPolicy {
            policyId: "widgets"; path: ["rows"]; keyField: "id"
            pruneUnknown: true; normalizeFields: true
        }]
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        store.updateRecordListPolicy("widgets", {ready:true,entries:[
            {key:"clock",available:true,retain:false,normalize:true,defaults:{compact:false,wide:"18446744073709551615"}}
        ]}, function(result) {
            if (result.status !== "projected" || root.observedCount !== 1 || root.observedCompact !== false
                    || rows.count !== 1 || rows.itemAt(0).text !== "clock") {
                root.failure = "housekeeping-consumer";
                return;
            }
            store.editRecordList("widgets", {generation:store.info(["rows"]).publicationGeneration,
                listAddress:[],edits:[{op:"set",index:0,field:"compact",value:true}]}, function(edit) {
                if (edit.status !== "admitted" || root.observedCompact !== true) {
                    root.failure = "housekeeping-field-edit";
                    return;
                }
                store.editRecordList("widgets", {generation:store.info(["rows"]).publicationGeneration,
                    listAddress:[],edits:[{op:"set",index:0,field:"wide",value:"18446744073709551614"}]}, function(wide) {
                    if (wide.status !== "admitted" || store.values.rows[0].wide !== "18446744073709551614") {
                        root.failure = "housekeeping-wide-domain";
                        return;
                    }
                    store.save(function(saved) {
                        if (saved.status !== "saved") root.failure = "housekeeping-save";
                        else root.finished = true;
                    });
                });
            });
        });
    }
    function runRestart() {
        store.updateRecordListPolicy("widgets", {ready:true,entries:[
            {key:"clock",available:true,retain:false,normalize:true,defaults:{compact:false,wide:"18446744073709551615"}}
        ]}, function(result) {
            if (result.status !== "projected" || root.observedCount !== 1 || root.observedCompact !== true
                    || store.values.rows[0].wide !== "18446744073709551614" || rows.itemAt(0).text !== "clock")
                root.failure = "housekeeping-restart";
            else root.finished = true;
        });
    }
}
