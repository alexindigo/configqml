import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    required property bool cold
    property bool finished: false
    property string failure: ""
    property bool started: false
    property var scope: null
    property var editor: null
    property var live: null
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "count"; valueType: ConfigValue.Integer; defaultValue: 0 }
                ConfigValue { key: "flag"; valueType: ConfigValue.Boolean; defaultValue: false }
                ConfigValue { key: "pages"; valueType: ConfigValue.StringList; defaultValue: [] }
            }
        }
        sources: [JsonFileSource { sourceId: "base"; prefix: ["settings"]; path: root.directory + "/base.json"; writable: true }]
    }
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue { valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "owner"; path: root.directory + "/owner.json"; writable: true }]
    }
    readonly property bool ready: base.ready && owner.ready
    onReadyChanged: if (ready && !started) Qt.callLater(run)
    Component.onCompleted: if (ready && !started) Qt.callLater(run)
    function check(ok: bool, reason: string): bool {
        if (!ok && failure === "") failure = reason;
        return ok;
    }
    function options(mode: string): var {
        return {mode: mode, base: scope, ownerPath: ["rows"],
            ownerGeneration: owner.info(["rows"]).publicationGeneration,
            entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"};
    }
    function run(): void {
        if (started) return;
        started = true;
        scope = base.scope(["settings"], false);
        if (cold) { consume(); return; }
        owner.openOverlay(options("editor"), function(opened) {
            if (!check(opened.status === "opened", "editor open")) return;
            editor = opened.editor;
            editor.values.flag = true;
            editor.apply(function(saved) {
                if (!check(saved.status === "saved" && saved.refreshState === "current", "Apply")) return;
                owner.openOverlay(options("live"), function(result) {
                    if (!check(result.status === "opened", "live open")) return;
                    live = result.editor;
                    if (!check(live.unset(["flag"]), "explicit unset")) return;
                    live.flush(function(flushed) {
                        if (!check(flushed.status === "saved", "flush")) return;
                        base.values.settings.count = 8;
                        base.values.settings.pages = ["later shared page"];
                        base.save(function(shared) {
                            if (check(shared.status === "saved", "base save")) consume();
                        });
                    });
                });
            });
        });
    }
    function consume(): void {
        owner.openOverlay(options("view"), function(opened) {
            if (!check(opened.status === "opened", "view open")) return;
            const view = opened.editor;
            if (!check(view.values.count === 0 && view.values.pages.length === 0,
                       "equal stored override lost future inheritance ownership")) return;
            view.detach();
            finished = true;
        });
    }
}
