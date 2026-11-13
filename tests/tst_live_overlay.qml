import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property string stage: "idle"
    property string action: ""
    property bool finished: false
    readonly property bool ready: base.ready && main.ready && lifecycle.ready
    property var live: null
    property var doomed: null
    property Connections lifecycleSignals: Connections {
        target: root.doomed
        function onStateChanged(): void {
            if (root.doomed && root.doomed.status === ConfigStore.Error)
                Qt.callLater(root.checkRemoval);
        }
    }
    readonly property int volume: live && live.ready ? live.values.volume : -1
    onVolumeChanged: {
        if (action === "refresh" && volume === 8) Qt.callLater(finishRefresh);
    }
    property ConfigStore base: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "volume"; valueType: ConfigValue.Integer; defaultValue: 1 }
                ConfigValue { key: "lastResult"; valueType: ConfigValue.Record; defaultValue: ({n: 0})
                    ConfigValue { key: "n"; valueType: ConfigValue.Integer }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "base"; prefix: ["settings"]; path: root.directory + "/base.json"; writable: true }]
    }
    property ConfigStore main: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: []
                ConfigValue { valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "main"; path: root.directory + "/main.json"; writable: true }]
    }
    property ConfigStore lifecycle: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: [{id: "x"}]
                ConfigValue { valueType: ConfigValue.Record
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "lifecycle"; path: root.directory + "/lifecycle.json"; writable: true }]
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = message;
        return ok;
    }
    function options(): var {
        return {mode: "live", base: base.scope(["settings"], false), ownerPath: ["rows"],
                ownerGeneration: main.info(["rows"]).publicationGeneration,
                entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
                overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
                differencePolicy: "top_level_omit_equal"};
    }
    function run(): void {
        stage = "runtime writer";
        main.openOverlay(options(), function(opened) {
            if (!check(opened.status === "opened", "open:" + opened.error.code)) return;
            live = opened.editor;
            let rejected = false;
            try { live.values.volume = "bad"; } catch (error) { rejected = true; }
            if (!check(rejected, "synchronous validation")) return;
            for (let n = 1; n <= 39; ++n) {
                live.values.lastResult = {n: n};
                if (!check(live.values.lastResult.n === n && !live.busy, "read after write:" + n)) return;
            }
            live.values.volume = 7;
            live.save(function(saved) {
                if (!check(saved.status === "saved" && saved.publications.length === 1
                           && saved.publications[0].success && live.values.lastResult.n === 39,
                           "runtime save:" + saved.error.code)) return;
                stage = "equal-to-base and unset";
                live.values.volume = 1;
                live.values.volume = 2;
                if (!check(live.unset(["volume"]) && live.values.volume === 1, "unset inheritance")) return;
                live.flush(function(flushed) {
                    if (!check(flushed.status === "saved", "runtime flush:" + flushed.error.code)) return;
                    stage = "foreign refresh";
                    action = "foreign";
                });
            });
        });
    }
    function foreignWritten(): void {
        action = "refresh";
        base.reload(function(result) {
            if (!check(result.status === "reloaded", "base reload")) return;
            if (volume === 8) finishRefresh();
        });
    }
    function finishRefresh(): void {
        if (action !== "refresh") return;
        action = "done";
        if (!check(live.values.lastResult.n === 39, "refresh retained override")) return;
        live.flush(function(result) {
            if (!check(result.status === "nothing_to_save", "refresh did not write")) return;
            main.close(function(closed) {
                if (!check(closed.status === "nothing_to_save" && live.status === ConfigStore.Error
                           && live.error === "overlay-owner", "owner close")) return;
                stage = "placement removal";
                let selected = options();
                selected.ownerGeneration = lifecycle.info(["rows"]).publicationGeneration;
                lifecycle.openOverlay(selected, function(opened) {
                    if (!check(opened.status === "opened", "lifecycle open")) return;
                    doomed = opened.editor;
                    lifecycle.values.rows = [];
                });
            });
        });
    }
    function checkRemoval(): void {
        if (!check(doomed.error === "overlay-identity", "placement identity")) return;
        let rejected = false;
        try { doomed.values.volume = 4; } catch (error) { rejected = true; }
        if (check(rejected, "removed placement rejects input")) {
            doomed.detach();
            finished = true;
        }
    }
    function runRestart(): void {
        main.openOverlay(options(), function(opened) {
            if (!check(opened.status === "opened", "restart open")) return;
            live = opened.editor;
            if (!check(volume === 8 && live.values.lastResult.n === 39, "restart consumer")) return;
            live.detach();
            if (check(live.status === ConfigStore.Closed, "detach")) finished = true;
        });
    }
}
