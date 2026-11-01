import QtQml
import QtQuick
import ConfigQml 1.0

Item {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready
    readonly property var cfg: store.values
    readonly property int observedCount: store.ready ? cfg.rows.length : -1
    readonly property var observedCompact: store.ready && cfg.rows.length === 1 ? cfg.rows[0].compact : undefined
    Repeater {
        id: rows
        model: root.ready ? root.store.listModel(["rows"]) : null
        delegate: Text {
            required property var modelData
            text: modelData.get("id").value
        }
    }
    Repeater {
        id: destination
        model: root.ready ? root.store.listModel(["destination"]) : null
        delegate: Text {
            required property var modelData
            required property string rowId
            text: rowId + ":" + modelData.get("wide").value
        }
    }
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; idField: "id"
                defaultValue: [{id:"hidden"}, {id:"clock", deprecated:3}]
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                    ConfigValue { key: "compact"; valueType: ConfigValue.Boolean; optional: true }
                    ConfigValue { key: "wide"; valueType: ConfigValue.UInt64; optional: true }
                }
            }
            ConfigValue {
                key: "destination"; valueType: ConfigValue.List; defaultValue: []; idField: "id"
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
        }, ConfigRecordListPolicy { policyId: "destination"; path: ["destination"]; keyField: "id" }]
        onStatusChanged: { if (status === ConfigStore.Error) root.failure = error; }
    }
    function run() {
        root.store.updateRecordListPolicy("widgets", {ready:true,entries:[
            {key:"clock",available:true,retain:false,normalize:true,defaults:{compact:false,wide:"18446744073709551615"}}
        ]}, function(result) {
            if (result.status !== "projected" || root.observedCount !== 1 || root.observedCompact !== false
                    || rows.count !== 1 || (rows.itemAt(0) as Text).text !== "clock") {
                root.failure = "housekeeping-consumer";
                return;
            }
            root.store.editRecordList("widgets", {generation:root.store.info(["rows"]).publicationGeneration,
                listAddress:[],edits:[{op:"set",index:0,field:"compact",value:true}]}, function(edit) {
                if (edit.status !== "admitted" || root.observedCompact !== true) {
                    root.failure = "housekeeping-field-edit";
                    return;
                }
                root.store.editRecordList("widgets", {generation:root.store.info(["rows"]).publicationGeneration,
                    listAddress:[],edits:[{op:"set",index:0,field:"wide",value:"18446744073709551614"}]}, function(wide) {
                    if (wide.status !== "admitted" || root.cfg.rows[0].wide !== "18446744073709551614") {
                        root.failure = "housekeeping-wide-domain";
                        return;
                    }
                    root.transferAndSave();
                });
            });
        });
    }
    function transferAndSave() {
        const generation = root.store.info(["rows"]).publicationGeneration;
        const target = root.store.info(["destination"]).publicationGeneration;
        for (const invalid of [
            {policyId:"destination",listAddress:[],generation:target,index:"0"},
            {policyId:"destination",listAddress:[{each:true}],generation:target,index:0},
            {policyId:"destination",listAddress:[],generation:1,index:0},
            {policyId:"destination",listAddress:[],generation:target,index:0,extra:true}
        ]) {
            let refused = false;
            try {
                root.store.editRecordList("widgets", {generation:generation,listAddress:[],edits:[{
                    op:"transfer",from:{policyId:"widgets",listAddress:[],generation:generation,index:0},to:invalid
                }]});
            } catch (error) { refused = String(error).indexOf("list-edit-operation") !== -1; }
            if (!refused) { root.failure = "transfer-invalid-endpoint"; return; }
        }
        root.store.editRecordList("widgets", {generation:generation,listAddress:[],edits:[{
            op:"transfer",from:{policyId:"widgets",listAddress:[],generation:generation,index:0},
            to:{policyId:"destination",listAddress:[],generation:target,index:0}
        }]}, function(moved) {
            if (moved.status !== "admitted" || root.observedCount !== 0 || rows.count !== 0
                    || destination.count !== 1 || (destination.itemAt(0) as Text).text !== "clock:18446744073709551614"
                    || root.store.listModel(["destination"]).indexOfRow("clock") !== 0
                    || root.store.info(["rows"]).publicationGeneration !== root.store.info(["destination"]).publicationGeneration) {
                root.failure = "transfer-consumer"; return;
            }
            root.store.save(function(saved) {
                if (saved.status !== "saved") root.failure = "housekeeping-save";
                else root.finished = true;
            });
        });
    }
    function runRestart() {
        root.store.updateRecordListPolicy("widgets", {ready:true,entries:[
            {key:"clock",available:true,retain:false,normalize:true,defaults:{compact:false,wide:"18446744073709551615"}}
        ]}, function(result) {
            if (result.status !== "projected" || root.observedCount !== 0 || rows.count !== 0
                    || root.cfg.destination[0].wide !== "18446744073709551614"
                    || destination.count !== 1 || (destination.itemAt(0) as Text).text !== "clock:18446744073709551614")
                root.failure = "housekeeping-restart";
            else root.finished = true;
        });
    }
}
