// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    required property string directory
    property var editor: null
    property var first: null
    property var second: null
    property var receipt: ({})
    property int calls: 0
    property int observed: store.ready ? store.values.a.n : -1
    property int other: store.ready ? store.values.b.n : -1
    property int preview: editor && editor.values.a.n !== undefined ? editor.values.a.n : -1
    property int rows: store.ready ? store.values.a.rows.length : -1
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {
            version: 1
            migrations: [{fromVersion: 0, transform: `function(raw) {
                if (Object.prototype.hasOwnProperty.call(raw, "old")) {
                    raw.n = raw.old
                    delete raw.old
                }
                raw.created = 12
                return raw
            }`}]
            ConfigGroup {
                key: "a"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 }
                ConfigValue {
                    key: "rows"; valueType: ConfigValue.List; defaultValue: []; idField: "key"
                    ConfigValue {
                        valueType: ConfigValue.Record
                        ConfigValue { key: "key"; valueType: ConfigValue.String }
                        ConfigValue { key: "title"; valueType: ConfigValue.String }
                        ConfigValue {
                            key: "nested"; valueType: ConfigValue.Record
                            ConfigValue { key: "n"; valueType: ConfigValue.Integer }
                        }
                    }
                }
                ConfigValue { key: "secret"; sensitive: true; defaultValue: "synthetic-default" }
                ConfigValue { key: "reference"; valueType: ConfigValue.SecretRef; defaultValue: "keyring:item" }
                ConfigValue { key: "local"; machineLocal: true; defaultValue: "device-default" }
                ConfigValue { key: "tolerant"; valueType: ConfigValue.Integer; defaultValue: 11; admissionPolicy: ConfigValue.Tolerant }
            }
            ConfigGroup {
                key: "b"
                ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 }
            }
        }
        sources: [
            JsonFileSource { sourceId: "a"; prefix: ["a"]; path: root.directory + "/a.json" },
            JsonFileSource { sourceId: "b"; prefix: ["b"]; path: root.directory + "/b.json" }
        ]
    }
    function edit(): bool {
        let prior = observed
        editor = store.draft([["a", "n"], ["b", "n"], ["a", "rows"]])
        editor.values.a.n = 7
        editor.values.b.n = 8
        return editor.dirty && preview === 7 && observed === prior
    }
    function apply(): void {
        editor.apply(function(result) { receipt = result; calls++ })
    }
    function save(): void {
        store.save(function(result) { receipt = result; calls++ })
    }
    function reload(): void {
        store.reload(function(result) { receipt = result; calls++ })
    }
    function exportProfile(): void {
        store.exportProfile("Portable", directory + "/profile.json", [],
                            function(result) { receipt = result; calls++ })
    }
    function importProfile(): void {
        store.importProfile(directory + "/profile.json", [],
                            function(result) { receipt = result; calls++ })
    }
    function change(): void {
        store.values.a.n = 17
        store.values.b.n = 18
    }
    function scopes(): bool {
        first = store.scope(["a"])
        second = store.scope(["b"])
        first.values.n = 27
        first.detach()
        second.values.n = 28
        let refused = false
        try { first.setUser(["n"], 99) } catch (error) { refused = true }
        return refused && second.values.n === 28
    }
    function budget(): void {
        editor = store.draft([["a", "rows"]])
        let values = []
        for (let i = 0; i < 1000; ++i)
            values.push({key: "row-" + i, title: "Title " + i, nested: {n: i}})
        editor.values.a.rows = values
        editor.apply(function(result) { receipt = result; calls++ })
    }
    function detachDuringCapture(): bool {
        first = store.scope(["a"])
        let row = {key: "a", nested: {n: 1}}
        Object.defineProperty(row, "title", {enumerable: true, get: function() {
            first.detach()
            return "Title"
        }})
        let refused = false
        try { first.setUser(["rows"], [row]) } catch (error) { refused = true }
        return refused && root.rows === 0
    }
}
