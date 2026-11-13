import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    required property int scenario
    property string failure: ""
    property string stage: "idle"
    property bool finished: false
    readonly property bool ready: store.ready
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue {
                    valueType: ConfigValue.Record; open: true
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                    ConfigValue { key: "label"; valueType: ConfigValue.String; optional: true }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
        recordListPolicies: [ConfigRecordListPolicy { policyId: "rows"; path: ["rows"]; keyField: "id" }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
        return ok;
    }
    function run(): void {
        stage = "edit";
        store.editRecordList("rows", {generation: store.info(["rows"]).publicationGeneration,
            listAddress: [], edits: [{op: "set", index: 0, field: "label", value: "edited"}]}, function(written) {
            if (!check(written.status === "admitted", "first native field edit admitted")) return;
            const captured = store.info(["rows"]);
            const displayed = JSON.stringify(store.copyValue(store.values.rows));
            function remove(saved): void {
                stage = "remove";
                const current = store.info(["rows"]);
                if (!check(displayed === JSON.stringify(store.copyValue(store.values.rows))
                           && current.pathGeneration === captured.pathGeneration
                           && current.intentRevision === captured.intentRevision
                           && current.contentRevision === captured.contentRevision,
                           "unchanged selection across own confirmation")) return;
                if (!check(scenario === 0 || saved.status === "saved", "own save confirmed")) return;
                const generation = scenario === 2 ? current.publicationGeneration : captured.publicationGeneration;
                store.editRecordList("rows", {generation: generation, listAddress: [],
                    edits: [{op: "remove", index: 0}]}, function(removed) {
                    if (!check(removed.status === "admitted" && store.values.rows.length === 0,
                               "original generation removal reaches consumer")) return;
                    store.save(function(result) {
                        if (check(result.status === "saved", "empty list confirmed on disk")) finished = true;
                    });
                });
            }
            if (scenario === 0) remove({status: "no-save"});
            else store.save(remove);
        });
    }
}
