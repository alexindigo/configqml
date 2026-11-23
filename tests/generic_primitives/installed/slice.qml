import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property bool finished: false
    property string failure: ""
    property bool started: false
    property var draft: null
    property var scope: null
    property var retained: null
    readonly property var values: store.values
    readonly property int bound: store.ready ? values.settings.count : -1
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "count"; valueType: ConfigValue.Integer; defaultValue: 3 }
                ConfigValue { key: "hidden"; valueType: ConfigValue.Integer; defaultValue: 2 }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
        onReadyChanged: if (ready) root.run()
    }
    function check(ok: bool, reason: string): bool {
        if (!ok && failure === "") failure = reason;
        return ok;
    }
    function run(): void {
        if (started) return;
        started = true;
        scope = store.scope(["settings"], false);
        values.settings.count = 4;
        values.settings.hidden = 8;
        store.save(function(saved) {
            if (!check(saved.status === "saved", "initial write")) return;
            draft = store.draft([["settings"]]);
            draft.values.settings.count = 12;
            draft.stageReset([["settings"]], function(staged) {
                const count = draft.readProjection.state(["settings", "count"]);
                const hidden = draft.readProjection.state(["settings", "hidden"]);
                if (!check(staged.status === "staged" && count.pending === "remove"
                    && count.targetPresent && count.candidateAvailable && count.value === 3
                    && hidden.value === 2 && bound === 4, "cancellable Reset")) return;
                if (!check(draft.discard() && draft.values.settings.count === 4, "Discard")) return;
                draft.stageReset([["settings"]], function(again) {
                    if (!check(again.status === "staged", "second Reset")) return;
                    draft.apply(function(write) {
                        if (!check(write.status === "saved", "Apply")) return;
                        scope.readProjection.synchronize(write.observation, function(read) {
                            retained = read;
                            if (!check(read.status === "observed" && bound === 3
                                && scope.values.count === 3 && read.snapshot.state(["count"]).value === 3,
                                "bound installed read")) return;
                            values.settings.count = 7;
                            if (!check(retained.snapshot.state(["count"]).value === 3,
                                "immutable snapshot")) return;
                            store.close(function(closed) {
                                if (!check(closed.status === "saved", "close")) return;
                                console.log("GENERIC_INSTALLED_QML_OK snapshot=3");
                                finished = true;
                            });
                        });
                    });
                });
            });
        });
    }
    Component.onCompleted: if (store.ready) run()
}
