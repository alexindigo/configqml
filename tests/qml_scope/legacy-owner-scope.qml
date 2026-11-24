// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQuick
import ConfigQml 1.0

// The standalone locker uses this same Loader/Component boundary and inherits
// ConfigStore through SettingsStore.qml. IDs must resolve to the enclosing
// consumer component, never to members added to the native scope object.
Item {
    id: root
    readonly property string witness: "outer"
    readonly property bool loaded: loader.status === Loader.Ready
    readonly property bool storeReady: store?.ready ?? false
    readonly property var store: loader.item
    readonly property bool rootsIntact: store?.outerRoot === root
    readonly property bool runtimeRootsIntact: store?.outerRuntimeRoot === runtimeRoot
    readonly property bool projectionsIntact: store?.outerProjection === readProjection
    readonly property bool abandonsIntact: store?.outerAbandon === abandonApply
    readonly property bool declarativeScopesIntact: schema.sameRoot && group.sameRoot
        && leaf.sameRoot && memory.sameRoot && file.sameRoot && projection.sameRoot && policy.sameRoot

    QtObject { id: runtimeRoot }
    QtObject { id: readProjection }
    QtObject { id: abandonApply }
    ConfigSchema { id: schema; readonly property bool sameRoot: root.witness === "outer" }
    ConfigGroup { id: group; readonly property bool sameRoot: root.witness === "outer" }
    ConfigValue { id: leaf; readonly property bool sameRoot: root.witness === "outer" }
    MemorySource { id: memory; readonly property bool sameRoot: root.witness === "outer" }
    JsonFileSource { id: file; readonly property bool sameRoot: root.witness === "outer" }
    ConfigProjection { id: projection; readonly property bool sameRoot: root.witness === "outer" }
    ConfigRecordListPolicy { id: policy; readonly property bool sameRoot: root.witness === "outer" }

    function returnedScopesIntact(): bool {
        const draft = store.draft([["section"]]);
        const scope = store.scope(["section"], false);
        const preview = draft.previewScope(["section"]);
        return [draft, scope, preview].every(owner => owner && owner.root === undefined
            && owner.runtimeRoot === undefined && owner.readProjection === undefined
            && owner.abandonApply === undefined);
    }

    Loader {
        id: loader
        sourceComponent: Component {
            ConfigStore {
                autoSave: false
                sources: [MemorySource {}]
                schema: ConfigSchema {
                    ConfigGroup {
                        key: "section"
                        ConfigValue { key: "sample"; defaultValue: true; valueType: ConfigValue.Boolean }
                    }
                }
                readonly property var outerRoot: root
                readonly property var outerRuntimeRoot: runtimeRoot
                readonly property var outerProjection: readProjection
                readonly property var outerAbandon: abandonApply
            }
        }
    }
}
