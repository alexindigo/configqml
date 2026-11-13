import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    required property string leg
    required property string mode
    property string failure: ""
    property string stage: "idle"
    property bool finished: false
    readonly property bool ready: baseStore.ready && owner.ready
    property var baseScope: null
    property ConfigStore baseStore: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "threshold"; valueType: ConfigValue.Real; defaultValue: 10 }
                ConfigValue { key: "hide"; valueType: ConfigValue.Boolean; defaultValue: false }
            }
        }
        sources: [JsonFileSource { sourceId: "base"; path: root.directory + "/base.json"; optional: true; writable: true }]
    }
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue {
                    valueType: ConfigValue.Record; open: true
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                    ConfigValue { key: "instanceSettings"; valueType: ConfigValue.Record; open: true; optional: true }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "rows"; path: root.directory + "/owner.json"; optional: true; writable: true }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
        return ok;
    }
    function run(): void {
        stage = leg + "-" + mode;
        baseScope = baseStore.scope(["settings"], false);
        if (leg === "json") { open(); return; }
        const overrides = {hide: false};
        if (leg !== "absent") overrides.threshold = leg === "integer" ? 17 : leg === "fraction" ? 17.5 : "17";
        owner.setUser(["rows"], [{id: "selected", instanceSettings: overrides, opaque: {keep: true}}], function(result) {
            if (check(result.status === "accepted", "owner setter admitted")) root.open();
        });
    }
    function open(): void {
        owner.openOverlay({mode: mode, base: baseScope, ownerPath: ["rows"],
            ownerGeneration: owner.info(["rows"]).publicationGeneration,
            entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "selected",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"}, function(result) {
            if (leg === "string") {
                if (check(result.status === "failed" && result.error.code === "type", "wrong type refused")) finished = true;
                return;
            }
            if (!check(result.status === "opened", "native overlay open " + result.error?.code)) return;
            const expected = leg === "absent" ? 10 : leg === "fraction" ? 17.5 : 17;
            if (!check(result.editor.values.threshold === expected && result.editor.values.hide === false,
                       "declared consumer sees expected numeric domain")) return;
            result.editor.detach();
            finished = true;
        });
    }
}
