// SPDX-License-Identifier: LGPL-3.0-or-later
import QtQml
import ConfigQml 1.0

QtObject {
    id: root
    property ConfigStore store: ConfigStore {
        schema: ConfigSchema {
            ConfigGroup {
                key: "bar"
                ConfigValue {
                    key: "n"
                    valueType: ConfigValue.Integer
                    defaultValue: 0
                    minimum: 0
                    maximum: 10
                }
                ConfigValue {
                    key: "mode"
                    valueType: ConfigValue.String
                    defaultValue: "on"
                    choices: ["on", "off"]
                }
                ConfigValue {
                    key: "widgets"
                    valueType: ConfigValue.List
                    defaultValue: [3, 1, 2]
                    ConfigValue {
                        valueType: ConfigValue.Integer
                    }
                }
                ConfigValue {
                    key: "wide"
                    valueType: ConfigValue.Int64
                    defaultValue: "9007199254740993"
                }
                ConfigValue {
                    key: "huge"
                    valueType: ConfigValue.UInt64
                    defaultValue: "18446744073709551615"
                }
                ConfigValue {
                    key: "wideList"
                    valueType: ConfigValue.List
                    defaultValue: ["1", "9007199254740993"]
                    ConfigValue {
                        valueType: ConfigValue.Int64
                    }
                }
                ConfigValue {
                    key: "safe"
                    valueType: ConfigValue.Integer
                    defaultValue: 9007199254740991
                }
                ConfigValue {
                    key: "nullable"
                    valueType: ConfigValue.String
                    nullable: true
                    defaultValue: null
                }
                ConfigValue {
                    key: "url"
                    valueType: ConfigValue.Url
                    defaultValue: Qt.resolvedUrl("https://example.test/a")
                }
                ConfigValue {
                    key: "real"
                    valueType: ConfigValue.Real
                    defaultValue: 0.5
                }
                ConfigValue {
                    key: "rows"
                    valueType: ConfigValue.List
                    defaultValue: [
                        {
                            id: "a",
                            top: 0,
                            items: [1]
                        }
                    ]
                    ConfigValue {
                        valueType: ConfigValue.Record
                        ConfigValue {
                            key: "id"
                            valueType: ConfigValue.String
                        }
                        ConfigValue {
                            key: "top"
                            valueType: ConfigValue.Integer
                        }
                        ConfigValue {
                            key: "items"
                            valueType: ConfigValue.List
                            ConfigValue {
                                valueType: ConfigValue.Integer
                            }
                        }
                    }
                }
                ConfigValue {
                    key: "record"
                    valueType: ConfigValue.Record
                    defaultValue: ({
                            top: 0,
                            items: [3, 1, 2]
                        })
                    ConfigValue {
                        key: "top"
                        valueType: ConfigValue.Integer
                    }
                    ConfigValue {
                        key: "items"
                        valueType: ConfigValue.List
                        ConfigValue {
                            valueType: ConfigValue.Integer
                        }
                    }
                }
                ConfigGroup {
                    key: "margins"
                    ConfigValue {
                        key: "top"
                        valueType: ConfigValue.Integer
                        defaultValue: 0
                    }
                }
            }
        }
        sources: [
            MemorySource {
                layerId: "user"
                sourceId: "memory"
                writable: true
            },
            MemorySource {
                layerId: "runtime"
                sourceId: "overlay"
                kind: MemorySource.Runtime
                writable: false
            }
        ]
        onCompleted: function (result) {
            root.order.push("signal:" + result.requestId);
        }
    }
    readonly property var cfg: store.values
    property var nativeFacade: null
    property var destroyer: null
    readonly property var active: nativeFacade || (store.ready ? cfg.bar : null)
    readonly property int count: store.ready && active ? active.widgets.length : -1
    readonly property int observedN: store.ready && active ? active.n : -1
    readonly property string observedWide: store.ready ? cfg.bar.wide : ""
    property var detached: store.ready ? cfg.bar.widgets : []
    property var order: []
    property var callbackResults: []
    property string closedWide: ""
    property bool reenter: false
    property bool reentered: false
    property Connections listener: Connections {
        target: root.active
        enabled: root.reenter
        function onNChanged() {
            if (root.active.n === 1 && !root.reentered) {
                root.reentered = true;
                root.store.save(function (result) {
                    root.callbackResults.push(result);
                });
                root.active.n = 2;
            }
        }
    }
    function run(code: string): var {
        return eval(code);
    }
    function appendBoth(): void {
        active.widgets.push(4);
        active.widgets.push(5);
    }
    function saveNow(): void {
        store.save(function (result) {
            root.order.push("callback:" + result.requestId);
            root.callbackResults.push(result);
        });
    }
}
