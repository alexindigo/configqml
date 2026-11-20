import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    readonly property bool ready: store.ready
    property string failure: ""
    property bool finished: false
    property var draft: null
    readonly property var effective: store.values
    readonly property var shown: draft ? draft.values : effective
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "flag"; valueType: ConfigValue.Boolean; defaultValue: true }
            ConfigValue { key: "zero"; valueType: ConfigValue.Integer; defaultValue: 5 }
            ConfigValue { key: "text"; valueType: ConfigValue.String; defaultValue: "schema" }
            ConfigValue { key: "list"; valueType: ConfigValue.StringList; defaultValue: ["schema"] }
            ConfigValue { key: "record"; valueType: ConfigValue.Record; open: true; defaultValue: ({label: "schema"})
                ConfigValue { key: "label"; valueType: ConfigValue.String }
            }
            ConfigValue { key: "emptyRecord"; valueType: ConfigValue.Record; defaultValue: ({}) }
            ConfigValue { key: "map"; valueType: ConfigValue.Map; defaultValue: ({})
                ConfigValue { valueType: ConfigValue.String }
            }
            ConfigValue { key: "nullable"; valueType: ConfigValue.String; nullable: true; defaultValue: "schema" }
            ConfigValue { key: "optional"; valueType: ConfigValue.String; optional: true }
        }
        sources: [
            JsonFileSource { layerId: "legacy"; sourceId: "legacy"; writable: false; path: root.directory + "/shapes-legacy.json" },
            JsonFileSource { layerId: "user"; sourceId: "user"; writable: true; path: root.directory + "/shapes-user.json" }
        ]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
        return ok;
    }
    function inherited(value: var): bool {
        const record = store.copyValue(value.record);
        return value.flag === false && value.zero === 0 && value.text === ""
            && value.list.length === 0 && Object.keys(value.emptyRecord).length === 0
            && Object.keys(value.map).length === 0 && value.nullable === null
            && value.optional === undefined && record.label === "lower"
            && record.wide.decimal === "18446744073709551615"
            && Object.prototype.hasOwnProperty.call(record, "__proto__")
            && record["__proto__"].x === false && record.constructor === "own";
    }
    function run(): void {
        draft = store.draft([[]]);
        draft.stageReset([[]], function(result) {
            if (!check(result.status === "staged" && inherited(shown) && inherited(draft.preview)
                       && effective.zero === 9 && effective.optional === "chosen",
                       "installed bindings preserve empty/null/absence and exact own-key atoms")) return;
            draft.apply(function(saved) {
                finished = check(saved.status === "saved" && !draft.dirty && inherited(effective),
                                 "installed atomic removal reaches applied consumer");
            });
        });
    }
    function runRestart(): void {
        finished = check(inherited(effective), "cold installed shapes and presence");
    }
}
