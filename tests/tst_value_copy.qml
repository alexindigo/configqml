import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property bool finished: false
    property string stage: "idle"
    readonly property bool ready: store.ready && owner.ready
    readonly property var cfg: store.values
    readonly property var placements: owner.values
    property var draft: null
    property var view: null
    property var live: null
    property var editor: null
    property var tokens: []
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue { valueType: ConfigValue.Record; open: true
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                    ConfigValue { key: "label"; valueType: ConfigValue.String; optional: true }
                    ConfigValue { key: "knownNumber"; valueType: ConfigValue.Real; optional: true }
                }
            }
            ConfigValue { key: "monitors"; valueType: ConfigValue.Map; defaultValue: ({})
                ConfigValue { valueType: ConfigValue.Record; open: true
                    ConfigValue { key: "label"; valueType: ConfigValue.String; optional: true }
                }
            }
            ConfigGroup { key: "settings"
                ConfigValue { key: "object"; valueType: ConfigValue.Record; open: true; defaultValue: ({label: "base"})
                    ConfigValue { key: "label"; valueType: ConfigValue.String; optional: true }
                    ConfigValue { key: "knownNumber"; valueType: ConfigValue.Real; optional: true }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
        recordListPolicies: [ConfigRecordListPolicy { policyId: "rows"; path: ["rows"]; keyField: "id" }]
    }
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "rows"; valueType: ConfigValue.List; defaultValue: [{id: "x", label: "a"}, {id: "x", label: "b"}]
                ConfigValue { valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "owner"; path: root.directory + "/owner.json"; writable: true }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = stage + ":" + message;
        return ok;
    }
    function own(value: var, key: string): var { return Object.getOwnPropertyDescriptor(value, key).value; }
    function numbers(value: var): bool {
        return check(value.knownNumber === 123, "declared Real control")
                && check(typeof value.customField === "number" && value.customField === 123
                         && value.fraction === 1.25, "safe numeric members")
                && check(value.wide.decimal === "18446744073709551615", "wide token")
                && check(Object.isFrozen(value.wide), "wide frozen")
                && check(value.overflow.decimal === "1e400", "overflow token")
                && check(Object.isFrozen(value.overflow), "overflow frozen")
                && check(value.longDecimal.decimal === "1.2345678901234567890123456789", "long decimal token");
    }
    function options(mode: string, base: var): var {
        return {mode: mode, base: base, ownerPath: ["rows"],
                ownerGeneration: owner.info(["rows"]).publicationGeneration,
                entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
                overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
                differencePolicy: "top_level_omit_equal"};
    }
    function run(): void {
        stage = "namespace and models";
        if (!numbers(cfg.rows[0]) || !numbers(cfg.settings.object) || !numbers(own(cfg.monitors, "__proto__"))) return;
        const model = store.listModel(["rows"]);
        const node = model.data(model.index(0, 0), Qt.UserRole);
        if (!check(node.get("customField").value === 123 && node.get("fraction").value === 1.25
                   && node.get("wide").value.decimal === "18446744073709551615"
                   && Object.isFrozen(node.get("wide").value), "list model unknown values")) return;
        const keyed = store.keyedModel(["monitors"]);
        const keyedNode = keyed.data(keyed.index(keyed.indexOfRow("__proto__"), 0), Qt.UserRole);
        if (!check(keyedNode.get("overflow").value.decimal === "1e400"
                   && Object.isFrozen(keyedNode.get("overflow").value), "keyed model atoms")) return;
        stage = "deep copy";
        const prefix = store.info(["monitors"]).latestInput;
        const map = store.copyValue(cfg.monitors);
        const record = store.copyValue(own(cfg.monitors, "__proto__"));
        if (!check(Object.getPrototypeOf(map) === null && Object.getPrototypeOf(record) === null
                   && Object.getPrototypeOf(record.nested) === null, "detached null-prototype dictionaries")) return;
        if (!numbers(record)) return;
        record.label = "edited";
        record.nested.__proto__.keep = true;
        record.items[0].label = "changed child";
        record.items.push({label: "added child"});
        if (!check(own(cfg.monitors, "__proto__").label === "original"
                   && own(own(cfg.monitors, "__proto__").nested, "__proto__").keep === false
                   && own(cfg.monitors, "__proto__").items.length === 1
                   && own(cfg.monitors, "__proto__").items[0].label === "child"
                   && store.info(["monitors"]).latestInput === prefix,
                   "deep mutation did not call the owner setter")) return;
        if (!check(own(record, "constructor").decimal === "1e400" && own(record, "prototype") === "kept",
                   "own prototype-like fields preserved")) return;
        let cycle = {}; cycle.self = cycle;
        let rejected = false;
        try { store.copyValue(cycle); } catch (error) { rejected = true; }
        if (!check(rejected, "cycle refused")) return;
        const fake = store.copyValue({decimal: "1e400"});
        if (!check(!Object.isFrozen(fake) && fake.decimal === "1e400", "ordinary records do not forge atoms")) return;
        map.__proto__ = record;
        cfg.monitors = map;
        if (!check(Number(store.info(["monitors"]).latestInput) === Number(prefix) + 1, "one owning assignment")) return;
        stage = "frozen draft";
        draft = store.draft([["settings"]]);
        if (!numbers(draft.values.settings.object)) return;
        stage = "initial preview";
        if (!numbers(draft.preview.settings.object)) return;
        const preview = store.scope(["settings"], false).copyValue(draft.preview.settings.object);
        preview.label = "preview";
        draft.values.settings.object = preview;
        stage = "edited preview";
        if (!numbers(draft.preview.settings.object) || !check(cfg.settings.object.label === "original", "draft detached")) return;
        stage = "record-list write";
        store.updateRecordListPolicy("rows", {ready: true, entries: [{key: "x", available: true,
            retain: false, normalize: false, defaults: {}}]}, function(updated) {
            if (!check(updated.status === "projected", "metadata")) return;
            store.editRecordList("rows", {generation: store.info(["rows"]).publicationGeneration,
                listAddress: [], edits: [{op: "set", index: 0, field: "label", value: "record-list"}]}, function(edited) {
                    if (!check(edited.status === "admitted", "record-list admission") || !numbers(cfg.rows[0])) return;
                    store.save(function(saved) {
                        if (!check(saved.status === "saved", "save copy and record-list")) return;
                        openViews();
                    });
                });
        });
    }
    function openViews(): void {
        stage = "overlays";
        const scope = draft.previewScope(["settings"]);
        owner.openOverlay(options("view", scope), function(a) {
            if (!check(a.status === "opened", "view open")) return;
            view = a.editor;
            owner.openOverlay(options("live", scope), function(b) {
                if (!check(b.status === "opened", "live open")) return;
                live = b.editor;
                owner.openOverlay(options("editor", store.scope(["settings"], false)), function(c) {
                    if (!check(c.status === "opened", "editor open")) return;
                    editor = c.editor;
                    if (!numbers(view.values.object) || !numbers(live.values.object) || !numbers(editor.values.object)) return;
                    tokens = [view.placementToken, live.placementToken, editor.placementToken];
                    const object = store.copyValue(live.values.object);
                    object.label = "instance";
                    live.values.object = object;
                    live.save(function(saved) {
                        if (!check(saved.status === "saved", "instance copy save") || !numbers(live.values.object)) return;
                        if (!check(tokens[0] === view.placementToken && tokens[1] === live.placementToken
                                   && tokens[2] === editor.placementToken, "override keeps placement token")) return;
                        memberAndMove();
                    });
                });
            });
        });
    }
    function memberAndMove(): void {
        stage = "placement tokens";
        let rows = owner.copyValue(placements.rows);
        rows[0].note = "indistinguishable-member-edit-or-replacement";
        placements.rows = rows;
        owner.flush(function(member) {
            if (!check(member.status === "saved" && tokens[0] === view.placementToken
                       && tokens[1] === live.placementToken && tokens[2] === editor.placementToken,
                       "member or indistinguishable replacement keeps token")) return;
            rows = owner.copyValue(placements.rows);
            rows.reverse();
            placements.rows = rows;
            owner.flush(function(moved) {
                if (!check(moved.status === "saved" && tokens[0] !== view.placementToken
                           && tokens[1] !== live.placementToken && tokens[2] !== editor.placementToken,
                           "same-ID reorder changes every mode token")) return;
                draft.discard();
                store.close(function(closed) {
                    if (!check(closed.status === "nothing_to_save", "store close")) return;
                    owner.close(function(done) {
                        if (check(done.status === "nothing_to_save", "owner close")) finished = true;
                    });
                });
            });
        });
    }
    function runRestart(): void {
        stage = "restart";
        if (numbers(cfg.rows[0]) && numbers(own(cfg.monitors, "__proto__"))
                && check(own(cfg.monitors, "__proto__").label === "edited"
                         && own(own(cfg.monitors, "__proto__").nested, "__proto__").keep === true,
                         "cold consumer sees detached copy assignment")) finished = true;
    }
}
