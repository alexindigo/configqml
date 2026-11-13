// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlevent_p.h"
#include "configqmlstore.h"
#include "storeprobe.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QtTest>
#include <private/qv4engine_p.h>
#include <private/qv4mm_p.h>

using namespace configqml;

class Replay : public QObject {
    Q_OBJECT
signals:
    void value(const QVariantMap &event);
};

class QmlCompactEventsTest : public QObject {
    Q_OBJECT
private slots:
    void effectsOnlyHeap() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        component.setData(R"QML(import QtQml
import ConfigQml 1.0
QtObject {
    id: root
    property int seen: 0
    property int effectsRead: 0
    property var last: null
    property ConfigStore store: ConfigStore {
        autoSave: false
        schema: ConfigSchema {}
        onProjectionEffects: function(event) {
            root.effectsRead += event.effects.length;
            root.seen += 1;
            root.last = event;
        }
    }
    function control(event) { root.effectsRead += event.effects.length; }
})QML",
                          QUrl("file:///compact-events.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root);
        auto *store = root->property("store").value<ConfigQmlStore *>();
        QTRY_VERIFY(store->ready());
        Replay replay;
        const auto wrapper = engine.newQObject(root.get());
        const auto signal = engine.newQObject(&replay).property("value");
        const auto connected =
            signal.property("connect").callWithInstance(signal, {wrapper.property("control")});
        QVERIFY2(!connected.isError(), qPrintable(connected.toString()));
        const QVariantMap old{{"runId", QString(3302, 'r')},
                              {"revision", QString(1616, 'v')},
                              {"sourceId", "file"},
                              {"projectionId", "p"},
                              {"effects", QVariantList{}}};
        auto *memory = engine.handle()->memoryManager;
        engine.collectGarbage();
        {
            QV4::GCCriticalSection critical(engine.handle());
            const auto before = memory->unmanagedHeapSize;
            emit replay.value(old);
            const auto charge = memory->unmanagedHeapSize - before;
            qInfo("Legacy empty-effects control unmanaged charge: %zu", charge);
            QVERIFY(charge >= 9800);
        }
        const ConfigSchema schema(ConfigDescriptor::group({}));
        const ConfigProjectionSpec spec{"p", {"file"}, {}, [](const ConfigProjectionInput &input) {
                                            return ConfigProjectionResult{
                                                input.values, input.compatibilityRaw, {}, {}};
                                        }};
        ConfigLayers layers(schema,
                            {{"user",
                              ConfigLayerKind::File,
                              ConfigAdmissionPolicy::FieldFallback,
                              {{"file", {}, {}, {}, QByteArray(32000, 'v')}}}},
                            {spec});
        ConfigWorkerPacket packet;
        packet.projectionEffects = layers.takeProjectionEffects();
        QCOMPARE(packet.projectionEffects.size(), 1);
        // Warm the engine-local factory independently of the measured delivery.
        qmlProjectionEvent(&engine, packet.projectionEffects[0]);
        engine.collectGarbage();
        {
            QV4::GCCriticalSection critical(engine.handle());
            const auto before = memory->unmanagedHeapSize;
            ConfigStoreTestAccess::inject(*store->core(), packet);
            const auto charge = memory->unmanagedHeapSize - before;
            qInfo("Compact empty-effects signal unmanaged charge: %zu", charge);
            QVERIFY2(charge <= 2048, "Unused identities must not charge the QML heap");
        }
        QCOMPARE(root->property("seen").toInt(), 1);
        QCOMPARE(root->property("effectsRead").toInt(), 0);
        const auto event = wrapper.property("last");
        QCOMPARE(event.property("revision").toString(), packet.projectionEffects[0].revision);
        QCOMPARE(event.property("runId").toString(), packet.projectionEffects[0].runId);
    }
    void metadataOnDemand() {
        QQmlEngine engine;
        auto calls = std::make_shared<QStringList>();
        auto retained = std::make_shared<int>(42);
        std::weak_ptr<int> lifetime = retained;
        auto event = qmlEvent(&engine, {"status", "evidence"},
                              [calls, retained](const QString &key) -> QVariant {
                                  calls->append(key);
                                  if (key == "status")
                                      return "saved";
                                  return QVariantList{*retained};
                              });
        retained.reset();
        QVERIFY(calls->isEmpty());
        engine.globalObject().setProperty("event", event);
        const auto result = engine.evaluate("event.status + ':' + event.status");
        QCOMPARE(result.toString(), QString("saved:saved"));
        QCOMPARE(*calls, QStringList{"status"});
        QVERIFY(!lifetime.expired());
        QCOMPARE(event.property("evidence").property(0).toInt(), 42);
        QCOMPARE(*calls, (QStringList{"status", "evidence"}));
        QCOMPARE(engine.evaluate("Object.keys(event).sort().join(',')").toString(),
                 QString("evidence,status"));
        event = {};
        engine.globalObject().setProperty("event", QJSValue());
        engine.collectGarbage();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(lifetime.expired());
    }
    void payloadKinds_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("completed-autosave-migration-overlay-callback") << 0;
        QTest::newRow("accepted-source") << 1;
        QTest::newRow("projection") << 2;
        QTest::newRow("source-error-warning") << 3;
    }
    void payloadKinds() {
        QFETCH(int, kind);
        QQmlEngine engine;
        ConfigResult receipt;
        receipt.status = "saved";
        ConfigSourceAccepted accepted;
        accepted.origin = "external";
        ConfigProjectionEffectsEvent projection{"run", "p", "s", "revision", {}};
        ConfigError warning{"warning", "s", "read"};
        const auto make = [&] {
            switch (kind) {
            case 0:
                return qmlResultEvent(&engine, receipt);
            case 1:
                return qmlAcceptedEvent(&engine, accepted,
                                        ConfigSchema(ConfigDescriptor::group({})));
            case 2:
                return qmlProjectionEvent(&engine, projection);
            default:
                return qmlErrorEvent(&engine, warning);
            }
        };
        make();
        for (int i = 0; i < 4096; ++i) {
            const auto id = QString::number(i);
            receipt.coveredRevisions[id] = {true, QByteArray(64, 'v')};
            receipt.coverage.append({{"path", id}, id, quint64(i), quint64(i)});
            receipt.evidence.append({id, QString(4096, 'd'), id, "read", "foreign", quint64(i)});
            accepted.sources.append({id, {true, QByteArray(64, 'v')}});
            accepted.changedPaths.append({"path", id});
            projection.effects.append({id, 0, "action", {{"payload", QString(4096, 'a')}}});
        }
        warning.evidence = receipt.evidence;
        engine.collectGarbage();
        QJSValue event;
        {
            QV4::GCCriticalSection critical(engine.handle());
            const auto before = engine.handle()->memoryManager->unmanagedHeapSize;
            event = make();
            const auto charge = engine.handle()->memoryManager->unmanagedHeapSize - before;
            qInfo("Payload kind %d, 4096 metadata entries, unmanaged charge: %zu", kind, charge);
            QVERIFY2(charge <= 4096, "Event delivery must not scale with metadata size");
        }
        // Explicit access retains every item of the original snapshot.
        receipt.coveredRevisions.clear();
        accepted.sources.clear();
        projection.effects.clear();
        warning.evidence.clear();
        const auto key = kind == 0   ? "coveredRevisions"
                         : kind == 1 ? "sources"
                         : kind == 2 ? "effects"
                                     : "evidence";
        const auto values = event.property(key);
        if (kind == 0)
            QCOMPARE(values.property("4095").toString(), QString(64, 'v'));
        else
            QCOMPARE(values.property("length").toInt(), 4096);
    }
};
QTEST_GUILESS_MAIN(QmlCompactEventsTest)
#include "test_qml_compact_events.moc"
