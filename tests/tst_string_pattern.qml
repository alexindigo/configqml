import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    property string stage: "idle"
    property var editor: null
    property string rejectedPath: ""
    property string rejectedCode: ""
    readonly property string observed: main.ready ? main.values.word : "loading"
    readonly property string preview: editor && editor.ready ? editor.values.word : "loading"
    readonly property bool ready: main.ready && base.ready && invalid.status === ConfigStore.Error && badType.status === ConfigStore.Error
    property ConfigStore invalid: ConfigStore {
        autoSave: false
        schema: ConfigSchema { ConfigValue { key: "word"; valueType: ConfigValue.String; optional: true; stringPattern: "(" } }
    }
    property ConfigStore badType: ConfigStore {
        autoSave: false
        schema: ConfigSchema { ConfigValue { key: "word"; valueType: ConfigValue.Boolean; defaultValue: false; stringPattern: "false" } }
    }
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema { ConfigGroup {
            key: "settings"
            ConfigValue { key: "word"; valueType: ConfigValue.String; defaultValue: "OK"; stringPattern: "[A-Z]{2}" }
        } }
        sources: [MemorySource { sourceId: "base"; writable: true }]
    }
    property ConfigStore main: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "word"; valueType: ConfigValue.String; defaultValue: "OK"; stringPattern: "[A-Z]{2}" }
            ConfigValue {
                key: "tokens"; valueType: ConfigValue.Map; sensitive: true; defaultValue: ({})
                ConfigValue { valueType: ConfigValue.String; stringPattern: "[A-Z]{2}" }
            }
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: [{id: "x", label: "OK"}]
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                    ConfigValue { key: "label"; valueType: ConfigValue.String; stringPattern: "[A-Z]{2}" }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "main"; path: root.directory + "/pattern.json"; writable: true }]
        recordListPolicies: [ConfigRecordListPolicy { policyId: "rows"; path: ["rows"]; keyField: "id" }]
        onInputRejected: function(path, code) {
            root.rejectedPath = path.join("/"); root.rejectedCode = code;
        }
    }
    function check(condition: bool, description: string): bool {
        if (!condition && failure === "") failure = description;
        return condition;
    }
    function rejects(action: var, description: string): bool {
        let threw = false;
        try { action(); } catch (error) { threw = true; }
        return check(threw, description);
    }
    function run(): void {
        stage = "checked writers";
        if (!check(invalid.error === "invalid-string-pattern" && badType.error === "string-pattern-type", "QML schema constraints")) return;
        const before = main.info(["word"]).latestInput;
        if (!rejects(function() { main.values.word = "bad"; }, "property rejection")) return;
        if (!rejects(function() { main.values.rows = [{id:"x",label:"bad"}]; }, "nested rejection")) return;
        if (!rejects(function() { main.values.tokens = {"opaque-secret":"bad"}; }, "sensitive rejection")) return;
        if (!check(rejectedCode === "string-pattern" && rejectedPath === "tokens", "value-free owning-leaf path")) return;
        const scoped = main.scope([], true);
        if (!rejects(function() { scoped.setUser(["word"], "bad"); }, "scoped rejection")) return;
        const draft = main.draft([["word"]]);
        if (!rejects(function() { draft.values.word = "bad"; }, "draft rejection")) return;
        if (!rejects(function() { main.retainGroup({requestKey:"bad",paths:[["word"]],edits:[{path:["word"],present:true,value:"bad"}]}); }, "group rejection")) return;
        if (!rejects(function() { main.editRecordList("rows", {generation:main.info(["rows"]).publicationGeneration,listAddress:[],edits:[{op:"set",index:0,field:"label",value:"bad"}]}); }, "list field rejection")) return;
        if (!rejects(function() { main.editRecordList("rows", {generation:main.info(["rows"]).publicationGeneration,listAddress:[],edits:[{op:"insert",index:1,value:{id:"y",label:"bad"}}]}); }, "list insert rejection")) return;
        if (!check(observed === "OK" && main.info(["word"]).latestInput === before && !draft.dirty, "no invalid intent")) return;
        draft.values.word = "UP";
        draft.apply(function(applied) {
            stage = "draft admitted";
            if (!check(applied.status === "saved" && observed === "UP", "checked draft endpoint")) return;
            main.editRecordList("rows", {generation:main.info(["rows"]).publicationGeneration,listAddress:[],edits:[{op:"set",index:0,field:"label",value:"UP"}]}, function(edited) {
                if (!check(edited.status === "admitted", "valid list field")) return;
                root.openEditor();
            });
        });
    }
    function openEditor(): void {
        main.openOverlay({base:base.scope(["settings"],false),ownerPath:["rows"],
            ownerGeneration:main.info(["rows"]).publicationGeneration,entryPath:[{index:0}],
            identityPath:[{key:"id"}],identityValue:"x",overrideKey:"instanceSettings",
            mergePolicy:"recursive_objects_replace_lists",differencePolicy:"top_level_omit_equal"}, function(opened) {
            stage = "overlay admitted";
            if (!check(opened.status === "opened", "overlay open: " + opened.error.code)) return;
            editor = opened.editor;
            if (!rejects(function() { editor.values.word = "bad"; }, "overlay constraint")) return;
            if (!check(!editor.dirty && preview === "OK", "overlay invalid no intent")) return;
            editor.values.word = "HI";
            editor.apply(function(saved) {
                if (!check(saved.status === "saved" && saved.refreshState === "current" && preview === "HI" && main.values.rows[0].label === "UP", "overlay checked replacement")) return;
                editor.resetOverrides(function(reset) {
                    if (!check(reset.status === "saved" && preview === "OK", "checked reset replacement")) return;
                    finished = true;
                });
            });
        });
    }
    function runRestart(): void {
        check(observed === "UP" && main.values.rows.length === 1 && main.values.rows[0].label === "UP" && main.values.rows[0].instanceSettings === undefined, "pattern disk restart consumer");
        finished = failure === "";
    }
}
