import QtQml
import ConfigQml 1.0

// QtQml-only installed consumer of generated handles and pending replies. Its
// "plugins.clock" fields are unknown to the library at build time.
QtObject {
    id: root
    required property string directory
    property bool finished: false
    property string failure: ""
    property bool started: false
    property var edit: null
    property var committed: null
    property int stress: 0
    property var clock: null
    // A closed store's handles are invalid and present nothing.
    readonly property int bound: clock && clock.fields.maxVisible.present
                                 ? clock.fields.maxVisible.value : -1
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "plugins"
                ConfigGroup {
                    key: "clock"
                    ConfigValue { key: "maxVisible"; valueType: ConfigValue.Integer; defaultValue: 2 }
                    ConfigValue { key: "format"; valueType: ConfigValue.String; optional: true }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true }]
        onReadyChanged: if (ready) root.run()
    }
    function check(ok: bool, reason: string): bool {
        if (!ok && failure === "") failure = reason;
        return ok;
    }
    function then(reply: var, reason: string, next: var): void {
        reply.finished.connect(function () {
            if (check(!reply.isError, reason + ":" + (reply.isError ? reply.error.code : "")))
                next(reply);
        });
    }
    function run(): void {
        if (started) return;
        started = true;
        clock = store.root.scopes.plugins.scopes.clock;
        if (!check(clock.fields.format.valid && !clock.fields.format.present, "optional field")) return;
        if (!check(clock.capabilities.indexOf("edit") >= 0, "capabilities")) return;
        then(clock.fields.maxVisible.set(5), "live set", function () {
            if (!check(bound === 5, "read-your-writes")) return;
            then(clock.apply(), "selected apply", function (saved) {
                if (!check(saved.value.write !== undefined, "write facts")) return;
                then(clock.edit(), "edit", function (opened) {
                    edit = opened.value.handle;
                    then(edit.fields.maxVisible.remove(), "staged remove", function () {
                        if (!check(edit.fields.maxVisible.value === 2 && bound === 5, "inherited candidate")) return;
                        then(edit.discard(), "discard", function () {
                            if (!check(edit.fields.maxVisible.value === 5, "discarded")) return;
                            then(edit.fields.maxVisible.set(3), "staged set", function () {
                                then(edit.apply({observe: [clock]}), "apply", function (applied) {
                                    const seen = applied.value.observations[0].snapshot;
                                    if (!check(seen.state(["maxVisible"]).value === 3 && bound === 3,
                                        "observed view")) return;
                                    committed = applied.value.generation;
                                    edit = null; // Closing the editor keeps committed coverage.
                                    gc();
                                    stressReplies();
                                });
                            });
                        });
                    });
                });
            });
        });
    }
    function stressReplies(): void {
        // Settled replies are reclaimable; queued completion is never lost.
        let remaining = 200;
        for (let i = 0; i < 200; ++i) {
            const reply = clock.read();
            reply.finished.connect(function () {
                ++stress;
                if (--remaining === 0) {
                    gc();
                    finish();
                }
            });
        }
    }
    function finish(): void {
        then(clock.observe(committed), "observe after close", function () {
            const forged = clock.observe({delivery: "1"});
            forged.finished.connect(function () {
                if (!check(forged.isError && forged.error.code === "read-observation", "forged token"))
                    return;
                store.close(function (closed) {
                    if (!check(closed.status !== "failed" && stress === 200, "close")) return;
                    console.log("GENERIC_INSTALLED_HANDLES_QML_OK maxVisible=3");
                    finished = true;
                });
            });
        });
    }
    Component.onCompleted: if (store.ready) run()
}
