import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property string failure: ""
    property string stage: "idle"
    property bool finished: false
    readonly property bool ready: store.ready && placements.ready
    readonly property string startupError: "store=" + store.error + "; placements=" + placements.error
    property bool checking: false
    property bool expectedEnabled: true
    property bool expectedCompact: false
    property bool expectedArt: true
    property int ownerSignals: 0
    property var lateRows: []
    property bool lateEnabled: true
    property var earlyWatcher: null
    property Component earlyConnections: Component {
        Connections {
            required property var harness
            target: harness.ownerValues.settings
            function onRowsChanged(): void {
                if (harness.checking) harness.check(harness.current(harness.lateRows, harness.lateEnabled), "early Connections bound consumer");
            }
            function onEnabledChanged(): void {
                if (harness.checking) harness.check(harness.current(harness.lateRows, harness.lateEnabled), "early Connections bound consumer");
            }
        }
    }
    property var scope: null
    property var nested: null
    property var model: null
    property var draft: null
    property var draftScope: null
    property bool draftChecking: false
    property bool draftHasEdits: false
    property bool previewClosed: false
    property bool draftCompact: true
    property bool draftArt: false
    property bool draftEnabled: false
    property var view: null
    property var live: null
    property var editor: null
    property bool overlayChecking: false
    property bool applying: false
    readonly property var ownerValues: store.values
    readonly property var ownerRows: ready ? ownerValues.settings.rows : []
    readonly property bool ownerEnabled: ready ? ownerValues.settings.enabled : true
    readonly property var scopeRows: scope ? scope.values.settings.rows : []
    readonly property bool scopeEnabled: scope ? scope.values.settings.enabled : true
    readonly property var nestedRows: nested ? nested.values.rows : []
    readonly property bool nestedEnabled: nested ? nested.values.enabled : true
    readonly property var draftRows: draft ? draft.values.settings.rows : []
    readonly property var draftEnabledConsumer: draft ? draft.values.settings.enabled : undefined
    readonly property var previewRows: draft ? draft.preview.settings.rows : []
    readonly property var previewEnabled: draft ? draft.preview.settings.enabled : undefined
    readonly property var previewScopeRows: draftScope && draftScope.ready ? draftScope.values.rows : []
    readonly property var previewScopeEnabled: draftScope && draftScope.ready ? draftScope.values.enabled : undefined
    readonly property var viewRows: view && view.ready ? view.values.rows : []
    readonly property var viewEnabled: view && view.ready ? view.values.enabled : undefined
    readonly property var liveRows: live && live.ready ? live.values.rows : []
    readonly property var liveEnabled: live && live.ready ? live.values.enabled : undefined
    readonly property var editorRows: editor && editor.ready ? editor.values.rows : []
    readonly property var editorEnabled: editor && editor.ready ? editor.values.enabled : undefined
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigGroup {
                key: "settings"
                ConfigValue { key: "enabled"; valueType: ConfigValue.Boolean; defaultValue: true }
                ConfigValue {
                    key: "rows"; valueType: ConfigValue.List; defaultValue: [{compact: false, art: true}]
                    ConfigValue {
                        valueType: ConfigValue.Record; open: true
                        ConfigValue { key: "compact"; valueType: ConfigValue.Boolean }
                        ConfigValue { key: "art"; valueType: ConfigValue.Boolean }
                    }
                }
            }
        }
        sources: [JsonFileSource { sourceId: "user"; path: root.directory + "/user.json"; writable: true },
            MemorySource { layerId: "runtime"; sourceId: "runtime"; kind: MemorySource.Runtime; writable: false }]
        onSettingValuesChanged: {
            if (root.checking) {
                ++root.ownerSignals;
                root.observe("owner-public");
            }
        }
    }
    property ConfigStore placements: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; defaultValue: [{id: "x"}]
                ConfigValue {
                    valueType: ConfigValue.Record; open: true
                    ConfigValue { key: "id"; valueType: ConfigValue.String }
                }
            }
        }
        sources: [MemorySource { sourceId: "placements"; writable: true }]
    }
    property Connections scopeSignals: Connections {
        target: root.scope
        function onSettingValuesChanged(): void { if (root.checking) root.observe("scope-public"); }
    }
    property Connections startupSignals: Connections {
        target: root.placements
        function onStatusChanged(): void { root.observeReady(); }
    }
    property Connections ownerStatusSignals: Connections {
        target: root.store
        function onStatusChanged(): void { root.observeReady(); }
    }
    property Connections nestedSignals: Connections {
        target: root.nested
        function onSettingValuesChanged(): void { if (root.checking) root.observe("nested-public"); }
    }
    property Connections modelSignals: Connections {
        target: root.model
        function onDataChanged(): void { if (root.checking) root.observe("model-dataChanged"); }
        function onRowsAboutToBeInserted(): void { if (root.checking) root.observe("model-aboutToInsert"); }
        function onRowsInserted(): void { if (root.checking) root.observe("model-inserted"); }
        function onGenerationChanged(): void { if (root.checking) root.observe("model-generationChanged"); }
    }
    property Connections draftSignals: Connections {
        target: root.draft
        function onSettingValuesChanged(): void { if (root.draftChecking) root.observeDraft("draft-values"); }
        function onDraftStateChanged(): void { if (root.draftChecking) root.observeDraft("draft-state"); }
    }
    property Connections previewSignals: Connections {
        target: root.draftScope
        function onSettingValuesChanged(): void { if (root.draftChecking) root.observeDraft("preview-values"); }
        function onStatusChanged(): void { if (root.draftChecking) root.observeDraft("preview-status"); }
    }
    property Connections viewSignals: Connections {
        target: root.view
        function onStateChanged(): void { root.observeOverlay("view"); }
    }
    property Connections liveSignals: Connections {
        target: root.live
        function onStateChanged(): void { root.observeOverlay("live"); }
    }
    property Connections editorSignals: Connections {
        target: root.editor
        function onStateChanged(): void { root.observeOverlay("editor"); }
    }
    function check(ok: bool, message: string): bool {
        if (!ok && failure === "") failure = stage + ": " + message;
        return ok;
    }
    function current(rows, enabled): bool {
        return enabled === expectedEnabled && rows.length === 1
            && rows[0].compact === expectedCompact && rows[0].art === expectedArt;
    }
    function observeReady(): void {
        if (store.ready && placements.ready && stage === "idle")
            check(current(ownerRows, ownerEnabled), "Ready handler bound namespace consumer");
    }
    function observe(signal: string): void {
        check(current(ownerValues.settings.rows, ownerValues.settings.enabled), signal + " existing owner getter");
        check(current(ownerRows, ownerEnabled), signal + " owner bound consumer");
        check(current(scope.values.settings.rows, scope.values.settings.enabled), signal + " existing scope getter");
        check(current(scopeRows, scopeEnabled), signal + " scope bound consumer");
        check(current(nested.values.rows, nested.values.enabled), signal + " existing nested getter");
        check(current(nestedRows, nestedEnabled), signal + " nested bound consumer");
        if (draftChecking) observeDraft(signal);
    }
    function run(): void {
        scope = store.scope([], false);
        nested = scope.scope(["settings"], false);
        observe("warm");
        earlyWatcher = earlyConnections.createObject(root, {harness: root});
        ownerValues.settings.rowsChanged.connect(function() {
            if (checking) check(current(lateRows, lateEnabled), "early namespace NOTIFY bound consumer");
        });
        ownerValues.settings.enabledChanged.connect(function() {
            if (checking) check(current(lateRows, lateEnabled), "early namespace NOTIFY bound consumer");
        });
        lateRows = Qt.binding(function() { return ownerValues.settings.rows; });
        lateEnabled = Qt.binding(function() { return ownerValues.settings.enabled; });
        checking = true;
        stage = "compact";
        expectedCompact = true;
        ownerValues.settings.rows[0].compact = true;
        stage = "art";
        expectedArt = false;
        ownerValues.settings.rows[0].art = false;
        stage = "enabled";
        expectedEnabled = false;
        ownerValues.settings.enabled = false;
        checking = false;
        check(ownerSignals === 3, "three property-origin signals");
        model = store.listModel(["settings", "rows"]);
        store.setUser(["settings", "enabled"], false, function(result) {
            if (!check(result.status === "accepted", "settled local writes")) return;
            stage = "runtime-model";
            expectedArt = true;
            checking = true;
            store.setRuntime(["settings", "rows"], [{compact: true, art: true}], function(runtime) {
                if (!check(runtime.status === "accepted", "runtime model publication")) return;
                stage = "clear-runtime";
                expectedArt = false;
                store.clearRuntime(["settings", "rows"], function(cleared) {
                    checking = false;
                    if (check(cleared.status === "accepted", "clear runtime publication")) startDraft();
                });
            });
        });
    }
    function observeDraft(signal: string): void {
        if (!draftChecking) return;
        const expectedRows = [{compact: draftCompact, art: draftArt}];
        check(draft.values.settings.enabled === draftEnabled && draftEnabledConsumer === draftEnabled
              && draftRows[0].compact === expectedRows[0].compact && draftRows[0].art === expectedRows[0].art,
              signal + " frozen draft getter/binding");
        const enabled = draftHasEdits ? draftEnabled : expectedEnabled;
        const compact = draftHasEdits ? draftCompact : expectedCompact;
        const art = draftHasEdits ? draftArt : expectedArt;
        check(draft.preview.settings.enabled === enabled && previewEnabled === enabled
              && previewRows[0].compact === compact && previewRows[0].art === art,
              signal + " preview getter/binding");
        if (previewClosed || (applying && !draftScope.ready))
            check(!draftScope.ready && draftScope.values.rows === undefined && previewScopeRows.length === 0
                  && previewScopeEnabled === undefined, signal + " closed preview getter/binding");
        else
            check(draftScope.values.enabled === enabled && previewScopeEnabled === enabled
                  && previewScopeRows[0].compact === compact && previewScopeRows[0].art === art,
                  signal + " preview scope getter/binding");
    }
    function startDraft(): void {
        stage = "draft";
        draft = store.draft([["settings"]]);
        draftScope = draft.previewScope(["settings"]);
        draftChecking = true;
        observeDraft("warm");
        draftHasEdits = true;
        draftCompact = false;
        draft.values.settings.rows[0].compact = false;
        draftArt = true;
        draft.values.settings.rows[0].art = true;
        draftEnabled = true;
        draft.values.settings.enabled = true;
        stage = "discard";
        draftHasEdits = false;
        draftCompact = expectedCompact;
        draftArt = expectedArt;
        draftEnabled = expectedEnabled;
        previewClosed = true;
        check(draft.discard(), "draft discard");
        observeDraft("after discard");
        draftChecking = false;
        draftScope = draft.previewScope(["settings"]);
        previewClosed = false;
        openOverlay("view");
    }
    function openOverlay(mode: string): void {
        stage = "open-" + mode;
        if (!placements.ready) { failure = "placement not Ready"; return; }
        placements.openOverlay({mode: mode, base: nested, ownerPath: ["rows"],
            ownerGeneration: placements.info(["rows"]).publicationGeneration,
            entryPath: [{index: 0}], identityPath: [{key: "id"}], identityValue: "x",
            overrideKey: "instanceSettings", mergePolicy: "recursive_objects_replace_lists",
            differencePolicy: "top_level_omit_equal"}, function(result) {
            if (!check(result.status === "opened", "overlay open")) return;
            if (mode === "view") { view = result.editor; openOverlay("live"); }
            else if (mode === "live") { live = result.editor; openOverlay("editor"); }
            else {
                editor = result.editor;
                overlayChecking = true;
                stage = "editor-write";
                editor.values.rows[0].compact = false;
                editor.values.rows[0].art = true;
                editor.values.enabled = true;
                check(editorEnabled === true, "editor immediate bound input");
                editor.apply(function(saved) {
                    if (!check(saved.status === "saved" && saved.refreshState === "current", "editor Apply")) return;
                    stage = "wait-editor";
                    advanceOverlays();
                });
            }
        });
    }
    function observeOverlay(kind: string): void {
        if (!overlayChecking) return;
        const handle = kind === "view" ? view : kind === "live" ? live : editor;
        if (!handle) return;
        const rows = kind === "view" ? viewRows : kind === "live" ? liveRows : editorRows;
        const enabled = kind === "view" ? viewEnabled : kind === "live" ? liveEnabled : editorEnabled;
        if (!handle.ready) {
            check(rows.length === 0 && enabled === undefined, kind + " closed guarded binding");
            return;
        }
        check(handle.values.enabled === enabled && rows[0].compact === handle.values.rows[0].compact
              && rows[0].art === handle.values.rows[0].art, kind + " state getter/binding");
        advanceOverlays();
    }
    function advanceOverlays(): void {
        if (stage === "wait-editor" && viewEnabled === true && liveEnabled === true) {
            stage = "live-write";
            live.values.rows[0].compact = true;
            live.values.rows[0].art = false;
            live.values.enabled = false;
            check(liveEnabled === false, "live immediate bound input");
            live.save(function(saved) {
                if (!check(saved.status === "saved", "live save")) return;
                stage = "wait-live";
                advanceOverlays();
            });
        } else if (stage === "wait-live" && viewEnabled === false && liveEnabled === false) {
            stage = "wait-base";
            expectedEnabled = true;
            checking = true;
            draftChecking = true;
            store.setUser(["settings", "enabled"], true);
        } else if (stage === "wait-base" && viewEnabled === true && liveEnabled === true) {
            checking = draftChecking = false;
            stage = "before-apply";
        }
    }
    function startApply(): void {
        stage = "draft-apply";
        draft = store.draft([["settings"]]);
        draftScope = draft.previewScope(["settings"]);
        draftHasEdits = true;
        draftCompact = false;
        draftArt = true;
        draftEnabled = false;
        draft.values.settings.rows = [{compact: false, art: true}];
        draft.values.settings.enabled = false;
        expectedCompact = false;
        expectedArt = true;
        expectedEnabled = false;
        applying = checking = draftChecking = true;
        draft.apply(function(result) {
            check(result.status === "saved", "draft Apply result");
            previewClosed = true;
            observe("draft Apply completion");
            observeDraft("draft Apply completion");
            checking = draftChecking = false;
            stage = "detach";
            editor.detach();
            live.detach();
            view.detach();
            check(viewEnabled === undefined && liveEnabled === undefined && editorEnabled === undefined,
                  "closed overlay bound consumers");
            finished = failure === "";
        });
    }
}
