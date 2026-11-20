import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    readonly property bool ready: shared.ready && owner.ready
    property string failure: ""
    property string stage: "idle"
    property string action: ""
    property bool finished: false
    property var draft: null
    property var scope: null
    property var view: null
    property var live: null
    readonly property var current: shared.values
    readonly property int frozen: draft ? draft.values.n : -1
    readonly property int previewed: draft ? draft.preview.n : -1
    readonly property int viewNumber: view ? view.values.n : -1
    readonly property int liveNumber: live ? live.values.n : -1
    property Connections draftSignals: Connections {
        target: root.draft
        function onDraftStateChanged(): void { Qt.callLater(root.transition); }
    }
    property Connections viewSignals: Connections {
        target: root.view
        function onStateChanged(): void { Qt.callLater(root.transition); }
    }
    property Connections liveSignals: Connections {
        target: root.live
        function onStateChanged(): void { Qt.callLater(root.transition); }
    }
    property ConfigStore shared: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 1 }
                ConfigValue { key: "other"; valueType: ConfigValue.Integer; defaultValue: 2 }
            }
        }
        sources: [
            JsonFileSource { layerId: "legacy"; sourceId: "legacy"; writable: false; prefix: ["settings"]; path: root.directory + "/legacy-reset.json"; fallbackFor: "user" },
            JsonFileSource { layerId: "user"; sourceId: "user"; writable: true; prefix: ["settings"]; path: root.directory + "/user.json" }
        ]
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
        sources: [MemorySource { sourceId: "placements"; writable: true }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
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
        const selected = shared.scope(["settings"]);
        draft = selected.draft([[]]);
        let refused = false;
        try { draft.reset(["n"], "user"); } catch (error) { refused = true; }
        if (!check(refused && frozen === 9 && !draft.dirty, "old reset stays refused")) return;
        refused = false;
        try { shared.stageReset([["settings", "n"]]); } catch (error) { refused = true; }
        if (!check(refused && current.settings.n === 9, "store cannot stage")) return;
        const readonlyScope = shared.scope(["settings"], false);
        refused = false;
        try { readonlyScope.stageReset([["n"]]); } catch (error) { refused = true; }
        if (!check(refused, "readonly scope cannot stage")) return;
        const detachedScope = shared.scope(["settings"]);
        detachedScope.detach();
        refused = false;
        try { detachedScope.stageReset([["n"]]); } catch (error) { refused = true; }
        if (!check(refused, "detached scope cannot stage")) return;
        stage = "stage-one";
        draft.stageReset([["n"]], function(result) {
            if (!check(result.operation === "stageReset" && result.status === "staged"
                       && frozen === 7 && previewed === 7 && current.settings.n === 9
                       && draft.values.other === 2 && draft.preview.other === 8,
                       "binding observes removal and whole-source fallback")) return;
            scope = draft.previewScope([]);
            let denied = false;
            try { scope.stageReset([["n"]]); } catch (error) { denied = true; }
            if (!check(denied, "preview scope cannot stage")) return;
            owner.openOverlay(options("view"), function(opened) {
                if (!check(opened.status === "opened", "view opened")) return;
                view = opened.editor;
                owner.openOverlay(options("live"), function(openedLive) {
                    if (!check(openedLive.status === "opened", "live opened")) return;
                    live = openedLive.editor;
                    let overlayDenied = false;
                    try { live.stageReset([["n"]]); } catch (error) { overlayDenied = true; }
                    if (!check(overlayDenied, "overlay cannot stage")) return;
                    if (!check(view.values.n === 7 && live.values.n === 7
                               && view.values.other === 8 && live.values.other === 8,
                               "overlay bases resolve removed final user key")) return;
                    stage = "pre-discard";
                    action = "check-before-apply";
                });
            });
        });
    }
    function checkedDisk(): void {
        stage = "foreign";
        action = "publish-foreign";
    }
    function reloadForeign(): void {
        action = "";
        shared.reload(function(result) {
            if (!check(result.status === "reloaded" && frozen === 7 && previewed === 17
                       && draft.conflicted && current.settings.n === 9,
                       "foreign publication prepares frozen/live bindings before callback")) return;
            Qt.callLater(transition);
        });
    }
    function reloadRestored(): void {
        action = "";
        shared.reload(function(result) {
            if (!check(result.status === "reloaded", "restore legacy fixture")) return;
            Qt.callLater(transition);
        });
    }
    function discardChecked(): void {
        if (!check(draft.discard() && frozen === 9 && previewed === 9
                   && current.settings.n === 9 && !draft.dirty, "discard stays cancelable")) return;
        stage = "discarded";
        Qt.callLater(transition);
    }
    function transition(): void {
        if (stage === "foreign" && previewed === 17 && viewNumber === 17 && liveNumber === 17) {
            if (!check(frozen === 7 && view.values.other === 18 && live.values.other === 18,
                       "foreign candidate reaches live overlay bases and bindings")) return;
            stage = "restore-foreign";
            draft.apply(function(result) {
                if (!check(result.status === "failed" && result.error.code === "draft-conflict"
                           && draft.dirty && frozen === 7, "live refresh never thaws Apply fences")) return;
                action = "restore-foreign";
            });
        } else if (stage === "restore-foreign" && action === "" && !draft.busy
                   && previewed === 7 && viewNumber === 7 && liveNumber === 7) {
            discardChecked();
        } else if (stage === "discarded" && view.error === "overlay-owner" && live.error === "overlay-owner") {
            stage = "stage-two";
            draft.stageReset([["n"]], function(result) {
                if (!check(result.status === "staged" && frozen === 7, "second stage")) return;
                stage = "set-refresh";
                draft.values.other = 12;
                if (!check(draft.busy, "set exposes candidate refresh busy")) return;
                Qt.callLater(transition);
            });
        } else if (stage === "set-refresh" && !draft.busy) {
            stage = "stage-three";
            if (!check(frozen === 1 && previewed === 1 && draft.values.other === 12,
                       "set on another leaf suppresses fallback with no previewScope")) return;
            draft.stageReset([["other"]], function(result) {
                if (!check(result.status === "staged" && frozen === 7 && previewed === 7
                           && draft.values.other === 8, "complete candidate refreshed again")) return;
                stage = "apply";
                draft.apply(function(saved) {
                    if (!check(saved.status === "saved" && !draft.dirty && !draft.busy
                               && current.settings.n === 7 && current.settings.other === 8,
                               "only Apply publishes raw removal")) return;
                    finished = true;
                });
            });
        }
    }
    function runRestart(): void {
        finished = check(current.settings.n === 7 && current.settings.other === 8,
                         "cold import resolves legacy fallback");
    }
}
