// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configdraft.h"
#include "configreadprojection.h"
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
ConfigReadResult read(ConfigReadProjection *projection) {
    QEventLoop loop;
    ConfigReadResult captured;
    projection->synchronize({}, &loop, [&](const ConfigReadResult &result) {
        captured = result;
        loop.quit();
    });
    loop.exec();
    return captured;
}
} // namespace

class GenericPerformanceTest : public QObject {
    Q_OBJECT
private slots:
    void unchangedSelectionRetainsCaptureIdentity() {
        ConfigStoreOptions options;
        options.schema = ConfigSchema(ConfigDescriptor::group(
            {{"count", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 4)}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto captured = store.readProjection()->snapshot();
        const auto selected = ConfigObservationAccess::select(captured, captured.paths());
        QCOMPARE(selected.observation(), captured.observation());
    }
    void batchAndNoOpScaling_data() {
        QTest::addColumn<int>("leaves");
        for (int count : {1, 32, 128, 512})
            QTest::newRow(qPrintable(QString::number(count))) << count;
    }
    void batchAndNoOpScaling() {
        QFETCH(int, leaves);
        QMap<QString, ConfigDescriptor> fields;
        QVariantMap raw;
        for (int i = 0; i < leaves; ++i) {
            const auto key = "field" + QString::number(i);
            fields[key] = ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 1);
            raw[key] = 4;
        }
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(ConfigDescriptor::group(fields));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, raw}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        auto *projection = draft->readProjection();
        resolutions(store); // Ordered worker/GUI delivery fence, not value polling.
        QJsonArray stageTimes, getterTimes;
        for (int sample = 0; sample < 9; ++sample) {
            const auto before = resolutions(store);
            QElapsedTimer timer;
            timer.start();
            const auto staged = compat::request(store, [&] { return draft->stageReset({{}}); });
            stageTimes.append(QString::number(timer.nsecsElapsed()));
            QCOMPARE(staged.status, QString("staged"));
            QCOMPARE(resolutions(store) - before, quint64(2)); // Frozen + live, not once per leaf.
            const auto snapshot = projection->snapshot();
            QCOMPARE(snapshot.paths().size(), leaves);
            QCOMPARE(snapshot.selectedState({"field0"}).candidateValue().toInt(), 1);
            const auto captures = resolutions(store);
            QSignalSpy values(&store, &ConfigStore::valuesChanged);
            for (int repeat = 0; repeat < 3; ++repeat)
                QCOMPARE(read(store.readProjection()).status(), QString("observed"));
            QCOMPARE(resolutions(store),
                     captures); // Control publications do not re-resolve drafts.
            QVERIFY(values.isEmpty());
            timer.restart();
            for (int query = 0; query < 10000; ++query)
                QVERIFY(draft->presentation({"field0"}).isValid());
            getterTimes.append(QString::number(timer.nsecsElapsed()));
            QCOMPARE(resolutions(store), captures); // Getters never acquire candidates.
            QVERIFY(draft->discard());
            resolutions(store);
        }
        const QJsonObject measurement{
            {"leaves", leaves}, {"stage_ns", stageTimes}, {"cached_getter_10000_ns", getterTimes}};
        qInfo().noquote() << "GENERIC_PERFORMANCE_SPAN"
                          << QJsonDocument(measurement).toJson(QJsonDocument::Compact);
    }
};
QTEST_GUILESS_MAIN(GenericPerformanceTest)
#include "test_generic_performance.moc"
