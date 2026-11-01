// SPDX-License-Identifier: LGPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import ConfigQml 1.0

Item {
    id: root
    width: 400
    height: 500
    property alias store: config
    readonly property var cfg: config.values
    property var projection: config.ready ? config.listModel(["rows"]) : null
    property var keyed: config.ready ? config.keyedModel(["monitors"]) : null
    property int created: 0
    property int destroyed: 0
    property bool useRepeater: true
    property alias list: view
    property alias repeater: repeated
    property alias monitorRepeater: monitorRepeated
    property alias numberRepeater: numberRepeated
    property var retained: null
    property string writeError: ""
    readonly property var numberModel: config.ready ? config.listModel(["numbers"]) : null
    ConfigStore {
        id: config
        autoSave: false
        sources: [MemorySource {}]
        schema: ConfigSchema {
            ConfigValue {
                key: "rows"; valueType: ConfigValue.List; idField: "key"
                defaultValue: [
                    {key: "a", title: "A", wide: "18446744073709551615", nested: {n: 1, items: [1, 2]}},
                    {key: "b", title: "B", wide: "2", nested: {n: 2, items: [3]}},
                    {key: "c", title: "C", wide: "3", nested: {n: 3, items: []}}
                ]
                ConfigValue {
                    valueType: ConfigValue.Record
                    ConfigValue { key: "key"; valueType: ConfigValue.String }
                    ConfigValue { key: "title"; valueType: ConfigValue.String }
                    ConfigValue { key: "wide"; valueType: ConfigValue.UInt64 }
                    ConfigValue {
                        key: "nested"; valueType: ConfigValue.Record
                        ConfigValue { key: "n"; valueType: ConfigValue.Integer }
                        ConfigValue {
                            key: "items"; valueType: ConfigValue.List
                            ConfigValue { valueType: ConfigValue.Integer }
                        }
                    }
                }
            }
            ConfigValue {
                key: "monitors"; valueType: ConfigValue.Map
                defaultValue: {"DP.1/a_..": "offline", "../ monitor": "retained"}
                ConfigValue { valueType: ConfigValue.String }
            }
            ConfigValue {
                key: "numbers"; valueType: ConfigValue.List; defaultValue: ["18446744073709551615", "2"]
                ConfigValue { valueType: ConfigValue.UInt64 }
            }
        }
    }
    component RowDelegate: Text {
        required property string rowId
        required property string title
        required property var modelData
        property var rowSnapshot: modelData
        property int serial: 0
    }
    component WritableRowDelegate: RowDelegate {
        required property var model
        signal roleWriteFailed(string message)
        function writeRole(): void {
            try { model.title = "hacked" }
            catch (error) { roleWriteFailed(String(error)) }
        }
    }
    Repeater {
        id: monitorRepeated
        model: root.keyed
        delegate: Text {
            required property string rowId
            required property string modelData
            text: rowId + ":" + modelData
        }
    }
    Repeater {
        id: numberRepeated
        model: root.numberModel
        delegate: Text {
            required property var modelData
            text: typeof modelData + ":" + modelData
        }
    }
    ListView {
        id: view
        width: 200; height: 500
        cacheBuffer: 25000
        model: root.projection
        delegate: RowDelegate {
            required property string wide
            required property var nested
            width: 200; height: 20
            text: rowId + ":" + title + ":" + nested.get("n").value
            property string exact: wide
            Component.onCompleted: serial = ++root.created
            Component.onDestruction: ++root.destroyed
        }
    }
    Column {
        x: 200
        Repeater {
            id: repeated
            model: root.useRepeater ? root.projection : null
            delegate: WritableRowDelegate {
                width: 200; height: 20
                text: rowId + ":" + title
                Component.onCompleted: serial = ++root.created
                Component.onDestruction: ++root.destroyed
                onRoleWriteFailed: function(message) { root.writeError = message }
            }
        }
    }
    function replaceRows(rows: var): void { cfg.rows = rows }
    function choose(index: int): void { view.currentIndex = index }
    function chooseKey(key: string): void { view.currentIndex = projection.indexOfRow(key) }
    function retain(): void { retained = (repeated.itemAt(1) as WritableRowDelegate).rowSnapshot.get("nested") }
    function protectedWrite(): string {
        try { retained.get("n").value = 999; return "no-error" }
        catch (error) { return String(error) }
    }
    function roleWrite(): void { (repeated.itemAt(0) as WritableRowDelegate).writeRole() }
    function nestedWide(): string {
        const item = repeated.itemAt(0) as WritableRowDelegate
        return typeof item.rowSnapshot.get("wide").value + ":" + item.rowSnapshot.get("wide").value
    }
    function monitorAt(index: int): string { return (monitorRepeated.itemAt(index) as Text).text }
    function numberAt(index: int): string { return (numberRepeated.itemAt(index) as Text).text }
    function directWrite(): string {
        try { projection.setData(projection.index(0, 0), "hack", 256); return "no-error" }
        catch (error) { return String(error) }
    }
    function rowAt(index: int): var {
        const item = repeated.itemAt(index) as WritableRowDelegate
        return item ? {key: item.rowId, text: item.text, serial: item.serial} : null
    }
    function makeRows(count: int): var {
        const rows = []
        for (let i = 0; i < count; ++i)
            rows.push({key: "row-" + i, title: "Title " + i, wide: String(i), nested: {n: i, items: []}})
        return rows
    }
    function populate(count: int): void { replaceRows(makeRows(count)) }
    function reverseRows(): void {
        const rows = cfg.rows.slice()
        rows.reverse()
        cfg.rows = rows
    }
    function rotateRows(): void {
        const rows = cfg.rows.slice()
        rows.push(rows.shift())
        cfg.rows = rows
    }
    function insertRow(): void {
        const rows = cfg.rows.slice()
        rows.splice(500, 0, {key: "inserted", title: "Inserted", wide: "17", nested: {n: 17, items: []}})
        cfg.rows = rows
    }
}
