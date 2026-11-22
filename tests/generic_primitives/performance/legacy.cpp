// SPDX-License-Identifier: LGPL-3.0-or-later
// Identical public-API fixture compiled against v0.5.3 and candidate headers.
// No invented baseline observation tokens or private old-client layouts.
#include "configdraft.h"
#include "configstorehandle.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using namespace configqml;
using D = ConfigDescriptor;
class LegacyPerformanceTest : public QObject {
    Q_OBJECT
private slots:
    void workflows_data() {
        QTest::addColumn<int>("leaves");
        QTest::addColumn<int>("sources");
        QTest::addColumn<int>("readers");
        for (int count : {1, 32, 128, 512})
            QTest::newRow(qPrintable(QString("leaves-%1").arg(count))) << count << 1 << 1;
        QTest::newRow("sources-4") << 128 << 4 << 1;
        QTest::newRow("sources-16") << 128 << 16 << 1;
        QTest::newRow("readers-8") << 128 << 4 << 8;
        QTest::newRow("readers-32") << 128 << 4 << 32;
    }
    void workflows() {
        QFETCH(int, leaves);
        QFETCH(int, sources);
        QFETCH(int, readers);
        QMap<QString, D> groups;
        QList<ConfigSource> shards;
        for (int source = 0; source < sources; ++source) {
            QMap<QString, D> fields;
            QVariantMap raw;
            for (int i = source; i < leaves; i += sources) {
                const auto name = "field" + QString::number(i);
                fields[name] = D::leaf(D::Type::SafeInteger, 1);
                raw[name] = 4;
            }
            const auto group = "group" + QString::number(source);
            groups[group] = D::group(fields);
            shards.append({group, {group}, raw});
        }
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group(groups));
        options.layers = {
            {"user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, shards, true}};
        ConfigStore store(options);
        // Baseline setup is not a measured post-ingress workload. Large legacy
        // schemas can take longer than QtTest's default five-second setup wait.
        QTRY_COMPARE_WITH_TIMEOUT(store.status(), ConfigStore::Status::Ready, 10000);
        QList<ConfigStoreHandle *> views;
        for (int i = 0; i < readers; ++i)
            views.append(store.scope({"group0"}, false));
        auto *draft = store.draft();
        QVERIFY(draft && draft->valid());
        const ConfigPath leaf{"group0", "field0"};
        QJsonArray reset, resetDispatch, setters, completion, getter, notification;
        for (int sample = 0; sample < 9; ++sample) {
            QEventLoop loop;
            quint64 id = 0;
            ConfigResult result;
            QElapsedTimer timer;
            auto connection =
                connect(&store, &ConfigStore::completed, &loop, [&](const ConfigResult &value) {
                    if (value.requestId == id) {
                        result = value;
                        loop.quit();
                    }
                });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            timer.start();
            id = draft->stageReset({{}});
            resetDispatch.append(QString::number(timer.nsecsElapsed()));
            loop.exec();
            reset.append(QString::number(timer.nsecsElapsed()));
            disconnect(connection);
            QVERIFY2(result.status == "staged", qPrintable(result.error.code));
            QCOMPARE(draft->presentation(leaf).toInt(), 1);
            QVERIFY(draft->discard());

            int notices = 0;
            QList<QMetaObject::Connection> connections;
            for (auto *view : views)
                connections.append(connect(view, &ConfigStoreHandle::valuesChanged, &store,
                                           [&](const QList<ConfigPath> &changed) {
                                               notices += changed.contains({"field0"});
                                           }));
            QEventLoop written;
            bool complete = false;
            qint64 consumerTime = 0;
            QTimer::singleShot(10000, &written, &QEventLoop::quit);
            timer.restart();
            const auto write =
                store.setUser(leaf, 10 + sample, &written, [&](const ConfigResult &value) {
                    consumerTime = timer.nsecsElapsed();
                    complete = value.status != "failed";
                    written.quit();
                });
            setters.append(QString::number(timer.nsecsElapsed()));
            QVERIFY(write.accepted);
            written.exec();
            QVERIFY(complete);
            completion.append(QString::number(consumerTime));
            QCOMPARE(notices, readers);
            notification.append(notices);
            for (auto connection : connections)
                disconnect(connection);
            for (auto *view : views)
                QCOMPARE(view->value({"field0"}).toInt(), 10 + sample);
            timer.restart();
            for (int query = 0; query < 10000; ++query)
                QVERIFY(draft->presentation(leaf).isValid());
            getter.append(QString::number(timer.nsecsElapsed()));
            // The next Reset needs an explicitly rebased frozen session after
            // the ordinary store write above. Do not turn conflict into timing.
            QVERIFY(draft->discard());
        }
        const QJsonObject measurement{{"leaves", leaves},
                                      {"sources", sources},
                                      {"readers", readers},
                                      {"reset_dispatch_ns", resetDispatch},
                                      {"reset_e2e_ns", reset},
                                      {"set_ingress_ns", setters},
                                      {"set_completion_including_consumers_ns", completion},
                                      {"cached_getter_10000_ns", getter},
                                      {"reader_value_notifications", notification}};
        qInfo().noquote() << "LEGACY_PERFORMANCE_SPAN"
                          << QJsonDocument(measurement).toJson(QJsonDocument::Compact);
    }
};
QTEST_GUILESS_MAIN(LegacyPerformanceTest)
#include "legacy.moc"
