import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property string stage: "idle"
    property string action: ""
    property bool finished: false
    readonly property bool ready: shared.ready && owner.ready
    readonly property var sharedValues: shared.values
    property var draft: null
    property var scope: null
    property var view: null
    property var live: null
    property var victim: null
    readonly property string label: draft ? draft.preview.settings.label : ""
    onLabelChanged: {
        if (stage === "refresh" && label === "foreign") Qt.callLater(afterForeign);
    }
    property Connections viewSignals: Connections {
        target: root.view
        function onStateChanged(): void { Qt.callLater(root.transitions); }
    }
    property Connections liveSignals: Connections {
        target: root.live
        function onStateChanged(): void { Qt.callLater(root.transitions); }
    }
    property ConfigStore shared: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "volume"; valueType: ConfigValue.Integer; defaultValue: 7 }
                ConfigValue { key: "label"; valueType: ConfigValue.String; defaultValue: "old" }
                ConfigValue { key: "atomic"; valueType: ConfigValue.Record; defaultValue: ({a: 1, b: 2})
                    ConfigValue { key: "a"; valueType: ConfigValue.Integer }
                    ConfigValue { key: "b"; valueType: ConfigValue.Integer }
                }
            }
            ConfigValue { key: "outside"; valueType: ConfigValue.String; defaultValue: "not selected" }
        }
        sources: [JsonFileSource { sourceId: "shared"; path: root.directory + "/shared.json"; writable: true }]
    }
    property ConfigStore owner: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue { key: "rows"; valueType: ConfigValue.List; defaultValue: [{id: "x"}]
                ConfigValue { valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "owner"; path: root.directory + "/owner.json"; writable: true }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
        return ok;
    }
    function options(mode: string, base: var): var {
        return {mode: mode, base: base, ownerPath: ["rows"],
                ownerGeneration: owner.info(["rows"]).publicationGeneration,
                entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
                overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
                differencePolicy: "top_level_omit_equal"};
    }
    function run(): void {
        draft = shared.draft([["settings"]]);
        draft.values.settings.volume = 9;
        draft.values.settings.atomic = {a: 9, b: 4};
        if (!check(draft.preview.settings.volume === 9 && sharedValues.settings.volume === 7
                   && draft.preview.settings.atomic.b === 4 && draft.preview.outside === undefined,
                   "shared preview 9, persistent 7, selected atomic edit")) return;
        let rejected = false;
        try { draft.preview.settings.volume = 99; } catch (error) { rejected = true; }
        if (!check(rejected && draft.preview.settings.volume === 9, "read-only namespace")) return;
        scope = draft.previewScope(["settings"]);
        rejected = false;
        try { scope.values.volume = 99; } catch (error) { rejected = true; }
        if (!check(rejected, "read-only scope")) return;
        owner.openOverlay(options("editor", scope), function(refused) {
            if (!check(refused.status === "failed" && refused.error.code === "overlay-selection", "frozen editor refused")) return;
            owner.openOverlay(options("view", scope), function(openedView) {
                if (!check(openedView.status === "opened", "view open")) return;
                view = openedView.editor;
                owner.openOverlay(options("live", scope), function(openedLive) {
                    if (!check(openedLive.status === "opened", "live open")) return;
                    live = openedLive.editor;
                    if (!check(view.values.volume === 9 && live.values.volume === 9, "inherited instance 9")) return;
                    live.values.volume = 11;
                    live.save(function(saved) {
                        if (!check(saved.status === "saved" && sharedValues.settings.volume === 7
                                   && draft.preview.settings.volume === 9 && live.values.volume === 11,
                                   "instance write targets only placement")) return;
                        if (!check(live.unset(["volume"]), "instance unset")) return;
                        live.flush(function(flushed) {
                            if (!check(flushed.status === "saved", "instance sparse flush")) return;
                            stage = "foreign";
                            action = "foreign";
                        });
                    });
                });
            });
        });
    }
    function foreignWritten(): void {
        action = "";
        stage = "refresh";
        shared.reload(function(result) {
            if (!check(result.status === "reloaded", "foreign reload")) return;
            if (label === "foreign") Qt.callLater(afterForeign);
        });
    }
    function afterForeign(): void {
        if (stage !== "refresh" || view.values.label !== "foreign" || live.values.label !== "foreign") return;
        stage = "conflict";
        if (!check(label === "foreign" && draft.values.settings.label === "old"
                   && draft.preview.settings.volume === 9 && sharedValues.settings.volume === 7
                   && view.values.volume === 9 && live.values.volume === 9,
                   "preview table after foreign change")) return;
        draft.apply(function(result) {
            if (!check(result.status === "failed" && scope.ready && view.ready && live.ready,
                       "frozen apply conflict unchanged")) return;
            stage = "discard";
            if (!check(draft.discard(), "discard")) return;
            Qt.callLater(transitions);
        });
    }
    function transitions(): void {
        if (stage === "refresh") { afterForeign(); return; }
        if (stage === "discard" && view.error === "overlay-owner" && live.error === "overlay-owner") {
            stage = "reopen";
            if (!check(scope.status === ConfigStore.Closed, "discard closed scope")) return;
            draft.values.settings.volume = 9;
            scope = draft.previewScope(["settings"]);
            owner.openOverlay(options("view", scope), function(opened) {
                if (!check(opened.status === "opened", "reopened preview")) return;
                view = opened.editor;
                stage = "apply";
                draft.apply(function(result) {
                    if (!check(result.status === "saved" && sharedValues.settings.volume === 9,
                               "apply persisted only shared edit")) return;
                    Qt.callLater(transitions);
                });
            });
        } else if (stage === "apply" && view.error === "overlay-owner") {
            stage = "victim";
            if (!check(scope.status === ConfigStore.Closed, "apply closed scope")) return;
            victim = shared.draft([["settings"]]);
            victim.values.settings.volume = 10;
            owner.openOverlay(options("live", victim.previewScope(["settings"])), function(opened) {
                if (!check(opened.status === "opened" && opened.editor.values.volume === 10, "victim preview")) return;
                live = opened.editor;
                stage = "destroy";
                action = "destroy";
            });
        } else if (stage === "destroy" && live.error === "overlay-owner") {
            action = "";
            stage = "done";
            if (check(sharedValues.settings.volume === 9, "destroy did not persist preview")) finished = true;
        }
    }
    function runRestart(): void {
        if (check(sharedValues.settings.volume === 9 && sharedValues.settings.label === "foreign"
                  && sharedValues.settings.atomic.a === 1, "restart persistent values, no discarded atomic edit"))
            finished = true;
    }
}
