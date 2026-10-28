// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configfacade.h"
#include "configqmlstore.h"
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>
using namespace configqml;
class DestroyConsumer : public QObject {
    Q_OBJECT
public:
    std::function<void()> action;
public slots:
    void fire() { action(); }
};
class WritableFacade : public ConfigFacade {
public:
    using ConfigFacade::ConfigFacade;
    using ConfigFacade::writeSetting;
};
class QmlLifetimeTest : public QObject {
    Q_OBJECT
private slots:
    void facadeRejectionDestroysEmitter() {
        ConfigStoreOptions options;
        options.schema = ConfigSchema(ConfigDescriptor::group(
            {{"n", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QPointer<WritableFacade> facade = new WritableFacade(&store);
        connect(facade, &ConfigFacade::inputRejected, this, [&] { delete facade.data(); });
        auto result = facade->writeSetting({"n"}, QString("bad"));
        QVERIFY(!result.accepted);
        QVERIFY(facade.isNull());
    }
    void qmlRejectDestroysEmitter() {
        QPointer<ConfigQmlStore> store = new ConfigQmlStore;
        connect(store, &ConfigQmlStore::inputRejected, this, [&] { delete store.data(); });
        store->setUser({"n"}, 1);
        QVERIFY(store.isNull());
    }
    void constructionSignals_data() {
        QTest::addColumn<QString>("boundary");
        for (const auto &name : {"schema", "values", "ready-error"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void constructionSignals() {
        QFETCH(QString, boundary);
        QPointer<ConfigQmlStore> store = new ConfigQmlStore;
        ConfigQmlSchema schema;
        ConfigQmlValue value;
        value.key = "n";
        value.valueType = ConfigQmlValue::Integer;
        value.defaultValue = 0;
        auto children = schema.children();
        children.append(&children, &value);
        if (boundary == "schema") {
            connect(store, &ConfigQmlStore::schemaChanged, this, [&] { delete store.data(); });
            store->setSchema(&schema);
        } else if (boundary == "values") {
            store->setSchema(&schema);
            connect(store, &ConfigQmlStore::valuesChanged, this, [&] { delete store.data(); });
            store->componentComplete();
        } else {
            connect(store, &ConfigQmlStore::statusChanged, this, [&] { delete store.data(); });
            store->componentComplete();
        }
        QTRY_VERIFY(store.isNull());
    }
    void destroyAtQmlBoundary_data() {
        QTest::addColumn<QString>("boundary");
        for (const auto &name : {"namespace-group", "namespace-setter", "store-rejection",
                                 "closing-status", "completion-signal", "result-callback"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void destroyAtQmlBoundary() {
        QFETCH(QString, boundary);
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_FIXTURE));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QPointer<ConfigQmlStore> store =
            qobject_cast<ConfigQmlStore *>(root->property("store").value<QObject *>());
        QTRY_VERIFY(store->ready());
        DestroyConsumer destroyer;
        destroyer.action = [&] { root.reset(); };
        if (boundary == "namespace-group") {
            auto *core = store->core();
            core->setUser({"bar", "n"}, 1);
            core->setUser({"bar", "widgets"}, QVariantList{9});
            QTRY_COMPARE(core->info({"bar", "widgets"}).workerCovered, quint64(2));
            auto *bar = store->values()->property("bar").value<QObject *>();
            QVERIFY(bar);
            QVERIFY(connect(bar, SIGNAL(nChanged()), &destroyer, SLOT(fire())));
            core->reset({"bar"}, "user");
        } else if (boundary == "namespace-setter") {
            store->close();
            QTRY_COMPARE(store->status(), ConfigQmlStore::Closed);
            auto *bar = store->values()->property("bar").value<QObject *>();
            QVERIFY(bar);
            const auto property =
                bar->metaObject()->property(bar->metaObject()->indexOfProperty("n"));
            connect(store, &ConfigQmlStore::inputRejected, &destroyer, &DestroyConsumer::fire);
            property.write(bar, 7);
        } else if (boundary == "store-rejection") {
            store->close();
            QTRY_COMPARE(store->status(), ConfigQmlStore::Closed);
            connect(store, &ConfigQmlStore::inputRejected, &destroyer, &DestroyConsumer::fire);
            store->setUser({"bar", "n"}, 1);
        } else if (boundary == "closing-status") {
            connect(store, &ConfigQmlStore::statusChanged, &destroyer, &DestroyConsumer::fire);
            store->close();
        } else if (boundary == "completion-signal") {
            connect(store, &ConfigQmlStore::completed, &destroyer, &DestroyConsumer::fire);
            store->save();
        } else {
            root->setProperty("destroyer", QVariant::fromValue<QObject *>(&destroyer));
            QVariant ignored;
            QMetaObject::invokeMethod(
                root.get(), "run", Q_RETURN_ARG(QVariant, ignored),
                Q_ARG(QString, QString("store.save(function(r){root.destroyer.fire()})")));
        }
        QTRY_VERIFY(!root);
        QVERIFY(store.isNull());
    }
    void readOnlyPropertiesAndExplicitWrites() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        component.setData(R"qml(
import QtQml
import ConfigQml 1.0
QtObject {
    property ConfigStore store: ConfigStore {
        schema: ConfigSchema { ConfigValue { key: "n"; valueType: ConfigValue.Integer; defaultValue: 0 } }
    }
    readonly property int seen: store.ready ? store.values.n : -1
    property var receipts: []
    function run(code: string): var { return eval(code) }
})qml",
                          QUrl("file:readonly.qml"));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = qobject_cast<ConfigQmlStore *>(root->property("store").value<QObject *>());
        QVERIFY(store);
        QTRY_VERIFY(store->ready());
        auto run = [&](const QString &code) {
            QVariant result;
            QMetaObject::invokeMethod(root.get(), "run", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QString, code));
            return result;
        };
        auto *values = store->values();
        const auto property =
            values->metaObject()->property(values->metaObject()->indexOfProperty("n"));
        QVERIFY(!property.isWritable());
        QVERIFY(!property.write(values, 7));
        QVERIFY(run("(function(){try{store.values.n=7;return false}catch(e){return true}})()")
                    .toBool());
        QVERIFY(run("(function(){try{store.setUser(['n'],8);return false}catch(e){return true}})()")
                    .toBool());
        QCOMPARE(root->property("seen").toInt(), 0);
        QCOMPARE(values->property("n").toLongLong(), 0);
        QVERIFY(store->core()->inputs().isEmpty());
        run("store.flush(function(r){receipts.push(r)});store.close(function(r){receipts.push(r)}"
            ")");
        QTRY_COMPARE(store->status(), ConfigQmlStore::Closed);
        const auto results = root->property("receipts").value<QJSValue>().toVariant().toList();
        QCOMPARE(results.size(), 2);
        for (const auto &value : results) {
            QCOMPARE(value.toMap()["status"].toString(), QString("nothing_to_save"));
            QVERIFY(value.toMap()["publications"].toList().isEmpty());
        }
    }
};
QTEST_GUILESS_MAIN(QmlLifetimeTest)
#include "test_qml_lifetime.moc"
