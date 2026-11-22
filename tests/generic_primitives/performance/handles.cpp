// SPDX-License-Identifier: LGPL-3.0-or-later
// Candidate-only spans: v0.5.3 has no handles, replies or causal read barriers.
#include "confighandle.h"
#include "compatfixture.h"
#include "storeprobe.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

using namespace configqml;
namespace {
quint64 resolutions(ConfigStore &store) {
    quint64 count = 0;
    QEventLoop loop;
    ConfigStoreTestAccess::run(
        store,
        [&](ConfigWorker &worker) { count = ConfigStoreTestAccess::candidateResolutions(worker); },
        &loop, [&] { loop.quit(); });
    loop.exec();
    return count;
}
std::unique_ptr<ConfigPendingReply> settle(ConfigPendingReply *reply) {
    std::unique_ptr<ConfigPendingReply> result(reply);
    QSignalSpy finished(reply, &ConfigPendingReply::finished);
    if (!reply->isFinished() && !finished.wait(10000))
        qFatal("reply timeout");
    return result;
}
} // namespace
class HandlePerformanceTest : public QObject {
    Q_OBJECT
private slots:
    void scaling_data() {
        QTest::addColumn<int>("leaves");
        for (int count : {1, 32, 128, 512})
            QTest::newRow(qPrintable(QString::number(count))) << count;
    }
    void scaling() {
        QFETCH(int, leaves);
        QMap<QString, ConfigDescriptor> fields;
        for (int i = 0; i < leaves; ++i)
            fields["field" + QString::number(i)] =
                ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 4);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(ConfigDescriptor::group(fields));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *root = store.root();
        QCOMPARE(root->findChildren<ConfigHandle *>().size(), 0);
        QCOMPARE(root->fields().size(), leaves);
        QCOMPARE(root->findChildren<ConfigHandle *>().size(),
                 0); // Schema is not an eager QObject graph.
        auto *field = root->scope({"field0"});
        QVERIFY(field->valid());
        QCOMPARE(root->findChildren<ConfigHandle *>().size(), 1);
        const auto before = resolutions(store);
        QJsonArray lookup, getter, reads, retention;
        QSignalSpy changes(field, &ConfigHandle::valueChanged);
        for (int sample = 0; sample < 9; ++sample) {
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 1000; ++i)
                QCOMPARE(root->scope({"field0"}), field);
            lookup.append(QString::number(timer.nsecsElapsed()));
            QCOMPARE(root->findChildren<ConfigHandle *>().size(), 1);
            timer.restart();
            for (int i = 0; i < 10000; ++i)
                QCOMPARE(field->value().toInt(), 4);
            getter.append(QString::number(timer.nsecsElapsed()));
            QCOMPARE(resolutions(store), before); // Lookup/getters never resolve a candidate.
            timer.restart();
            auto reply = settle(field->read());
            reads.append(QString::number(timer.nsecsElapsed()));
            QVERIFY2(!reply->isError(), qPrintable(reply->error().code()));
            QCOMPARE(reply->value().snapshot().paths().size(), 1);
            QCOMPARE(reply->value().snapshot().selectedState({}).value().toInt(), 4);
            const auto captured = reply->value();
            QPointer<ConfigPendingReply> lifetime(reply.get());
            timer.restart();
            // Detached immutable payloads do not keep the terminal QObject alive.
            reply.reset();
            QVERIFY(!lifetime);
            for (int i = 0; i < 1000; ++i)
                QCOMPARE(captured.snapshot().selectedState({}).value().toInt(), 4);
            retention.append(QString::number(timer.nsecsElapsed()));
            QCOMPARE(resolutions(store), before); // Read-only controls do not re-resolve.
            QCOMPARE(changes.size(), 0);          // No-op barriers never invent value transitions.
        }
        QCOMPARE(root->findChildren<ConfigPendingReply *>().size(), 0);
        const QJsonObject measurement{{"leaves", leaves},
                                      {"materialized_fields", 1},
                                      {"lookup_1000_ns", lookup},
                                      {"cached_getter_10000_ns", getter},
                                      {"selected_read_e2e_ns", reads},
                                      {"retained_snapshot_1000_ns", retention},
                                      {"extra_candidate_resolutions", 0},
                                      {"value_notifications", changes.size()}};
        qInfo().noquote() << "GENERIC_HANDLE_PERFORMANCE_SPAN"
                          << QJsonDocument(measurement).toJson(QJsonDocument::Compact);
    }
};
QTEST_GUILESS_MAIN(HandlePerformanceTest)
#include "handles.moc"
