import QtQml
import QtQuick
import ConfigQml 1.0

Item {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    readonly property bool ready: store.ready && identityBefore.ready && identityAfter.ready
    readonly property var cfg: store.values
    readonly property int observedCount: store.ready ? cfg.rows.length : -1
    readonly property var observedCompact: store.ready && cfg.rows.length === 1 ? cfg.rows[0].compact : undefined
    property IdentityAncestorCase identityBefore: IdentityAncestorCase {
        filePath: root.directory + "/identity-before.json"; ancestorId: "a-screens"
        onErrorChanged: { if (error) root.failure = error; }
    }
    property IdentityAncestorCase identityAfter: IdentityAncestorCase {
        filePath: root.directory + "/identity-after.json"; ancestorId: "z-screens"
        onErrorChanged: { if (error) root.failure = error; }
    }
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
                key: "screens"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "name"; valueType: ConfigValue.String }
                    ConfigValue {
                        key: "widgets"; valueType: ConfigValue.List; optional: true
                        ConfigValue {
                            valueType: ConfigValue.Record
                            ConfigValue { key: "id"; valueType: ConfigValue.String }
                            ConfigValue { key: "wide"; valueType: ConfigValue.UInt64; optional: true }
                        }
                    }
                }
            }
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
        }, ConfigRecordListPolicy { policyId: "destination"; path: ["destination"]; keyField: "id" },
        ConfigRecordListPolicy { policyId: "screens"; path: ["screens"]; keyField: "name" },
        ConfigRecordListPolicy {
            policyId: "screen-widgets"; path: ["screens"]; keyField: "id"
            listPath: [{each:true}, {key:"widgets"}]
        }]
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
            root.containersAndSave();
        });
    }
    function containersAndSave() {
        const generation = root.store.info(["screens"]).publicationGeneration;
        const at = [{field:"name", equals:"DP-1"}, {key:"widgets"}];
        const source = root.store.info(["destination"]).publicationGeneration;
        root.store.editRecordList("screen-widgets", {generation:generation, listAddress:at, edits:[
            {op:"copy", from:{policyId:"destination", listAddress:[], generation:source},
                to:{policyId:"screen-widgets", listAddress:at, generation:generation, ensure:true}},
            {op:"set", index:0, field:"wide", value:"18446744073709551613",
                at:{policyId:"screen-widgets", listAddress:at, generation:generation}}
        ]}, function(created) {
            if (created.status !== "admitted" || root.cfg.screens[0].name !== "DP-1"
                    || root.cfg.screens[0].widgets[0].wide !== "18446744073709551613") {
                root.failure = "containers-qml"; return;
            }
            root.store.save(function(saved) {
                if (saved.status !== "saved") root.failure = "housekeeping-save";
                else root.identityBefore.run(function(error) {
                    if (error) { root.failure = error; return; }
                    root.identityAfter.run(function(afterError) {
                        if (afterError) root.failure = afterError;
                        else root.finished = true;
                    });
                });
            });
        });
    }
    function runRestart() {
        root.store.updateRecordListPolicy("widgets", {ready:true,entries:[
            {key:"clock",available:true,retain:false,normalize:true,defaults:{compact:false,wide:"18446744073709551615"}}
        ]}, function(result) {
            if (result.status !== "projected" || root.observedCount !== 0 || rows.count !== 0
                     || root.cfg.destination[0].wide !== "18446744073709551614"
                     || root.cfg.screens[0].widgets[0].wide !== "18446744073709551613"
                    || destination.count !== 1 || (destination.itemAt(0) as Text).text !== "clock:18446744073709551614")
                root.failure = "housekeeping-restart";
            else root.identityBefore.runRestart(function(error) {
                if (error) { root.failure = error; return; }
                root.identityAfter.runRestart(function(afterError) {
                    if (afterError) root.failure = afterError;
                    else root.finished = true;
                });
            });
        });
    }
}
