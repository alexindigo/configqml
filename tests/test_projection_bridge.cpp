// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmldeclaration.h"
#include "compatfixture.h"
#include "confignumber.h"
#include <QJSEngine>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class ProjectionBridgeTest : public QObject {
    Q_OBJECT
private slots:
    void rejectedResults_data() {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("code");
        QTest::newRow("projection-source")
            << QString("not javascript") << QString("projection-source");
        QTest::newRow("projection-noncallable") << QString("42") << QString("projection-source");
        QTest::newRow("projection-script")
            << QString("function(i){throw new Error('private')}") << QString("projection-script");
        QTest::newRow("projection-result")
            << QString("function(i){return {}}") << QString("projection-result");
        QTest::newRow("projection-unknown-control")
            << QString("function(i){return "
                       "{values:{},compatibilityRaw:{},effects:[],diagnostics:[],extra:1}}")
            << QString("projection-result");
        QTest::newRow("projection-atom-root")
            << QString("function(i){return "
                       "{values:i.raw.a,compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-result");
        QTest::newRow("projection-cycle")
            << QString("function(i){var x={};x.x=x;return "
                       "{values:x,compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-cycle");
        QTest::newRow("projection-depth")
            << QString("function(i){var x={},r=x;for(var j=0;j<70;j++){x.x={};x=x.x}return "
                       "{values:r,compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-depth");
        QTest::newRow("projection-number")
            << QString("function(i){return "
                       "{values:{a:NaN},compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-number");
        QTest::newRow("projection-undefined")
            << QString("function(i){return "
                       "{values:{a:undefined},compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-result");
        QTest::newRow("projection-array-hole")
            << QString("function(i){return "
                       "{values:{x:Array(1)},compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-result");
        QTest::newRow("projection-live-result")
            << QString("function(i){return "
                       "{values:{x:function(){}},compatibilityRaw:{},effects:[],diagnostics:[]}}")
            << QString("projection-result");
        QTest::newRow("projection-effect-duplicate")
            << QString("function(i){var e={effectId:'x',version:0,action:'a',arguments:{}};return "
                       "{values:{},compatibilityRaw:{},effects:[e,e],diagnostics:[]}}")
            << QString("projection-effects");
        QTest::newRow("projection-effects-limit")
            << QString("function(i){return "
                       "{values:{},compatibilityRaw:{},effects:Array(65),diagnostics:[]}}")
            << QString("projection-effects");
        QTest::newRow("projection-effect-controls")
            << QString("function(i){return "
                       "{values:{},compatibilityRaw:{},effects:[{effectId:'x',version:'1',action:'"
                       "a',arguments:{}}],diagnostics:[]}}")
            << QString("projection-effects");
        QTest::newRow("projection-diagnostics-limit")
            << QString("function(i){return "
                       "{values:{},compatibilityRaw:{},effects:[],diagnostics:Array(65)}}")
            << QString("projection-diagnostics");
        QTest::newRow("projection-error-diagnostic")
            << QString("function(i){return "
                       "{values:{},compatibilityRaw:{},effects:[],diagnostics:[{code:'authored',"
                       "path:[],severity:'error'}]}}")
            << QString("projection-diagnostics");
    }
    void rejectedResults() {
        QFETCH(QString, body);
        QFETCH(QString, code);
        ConfigQmlProjection declaration;
        declaration.projectionId = "p";
        declaration.sourceIds = {"legacy"};
        declaration.transform = QJSValue(body);
        QList<ConfigDiagnostic> errors;
        const auto spec = declaration.compile(errors);
        QVERIFY(errors.isEmpty());
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"a", D::leaf(T::SafeInteger, 0)}}));
        o.layers = {{"legacy",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"legacy", {}, {{"a", 7}}}}}};
        o.projections = {spec};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QCOMPARE(store.error(), code);
        QVERIFY(!store.error().contains("private"));
    }
    void exactAtomsPresenceAndSpecialKeys() {
        ConfigQmlProjection declaration;
        declaration.projectionId = "p";
        declaration.sourceIds = {"legacy"};
        declaration.transform = QJSValue(QString(R"JS(function(i) {
            var has=Object.prototype.hasOwnProperty;
            if (Object.getPrototypeOf(i.raw)!==null || Object.getPrototypeOf(i.raw.__proto__)!==null
                || !has.call(i.raw,'__proto__') || i.raw.no!==false || i.raw.empty!==''
                || i.raw.nil!==null || has.call(i.raw,'absent') || i.source.presence!=='present'
                || i.priorVersion.present || i.schemaVersion!==0 || i.defaults.a.decimal!=='0'
                || i.lowerValues.a.decimal!=='0' || i.raw.wide.decimal!=='18446744073709551615')
                throw new Error('protocol');
            i.values.a=Number(i.raw.a.decimal)+1;
            return {values:i.values,compatibilityRaw:i.compatibilityRaw,effects:[],diagnostics:[]};
        })JS"));
        QList<ConfigDiagnostic> errors;
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"a", D::leaf(T::SafeInteger, 0)}}));
        const auto raw =
            decodeJsonValue("{\"a\":7,\"wide\":18446744073709551615,\"no\":false,\"empty\":\"\","
                            "\"nil\":null,\"__proto__\":{\"constructor\":0}}");
        o.layers = {{"legacy",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"legacy", {}, raw.value.toMap()}}}};
        o.projections = {declaration.compile(errors)};
        QVERIFY(errors.isEmpty());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"a"}).toInt(), 8);
    }
    void noLiveFunction() {
        QJSEngine engine;
        ConfigQmlProjection declaration;
        declaration.transform = engine.evaluate("(function(i){return i})");
        QList<ConfigDiagnostic> errors;
        declaration.compile(errors);
        QVERIFY(!errors.isEmpty());
        QCOMPARE(errors.first().code, QString("projection-declaration"));
    }
};
QTEST_GUILESS_MAIN(ProjectionBridgeTest)
#include "test_projection_bridge.moc"
