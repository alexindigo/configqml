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
    void emptyDraftUsesPublicationCut_data() { batchAndNoOpScaling_data(); }
    void emptyDraftRetainsRawTargetMembership() {
        using D = ConfigDescriptor;
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(D::Type::SafeInteger, 1)}}));
        ConfigSource absent{"missing", {}, {{"count", 4}}};
        absent.exists = false;
        options.layers = {
            {"user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {absent}, true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        QVERIFY(draft && draft->valid());
        resolutions(store);
        const auto retained = draft->previewProjection()->snapshot();
        const auto state = retained.selectedState({"count"});
        QCOMPARE(state.value().toInt(), 1);
        QVERIFY(state.targetPresent());
        QVERIFY(store.setUser({"count"}, 7).accepted);
        resolutions(store);
        const auto current = draft->previewProjection()->snapshot().selectedState({"count"});
        QCOMPARE(current.value().toInt(), 7);
        QVERIFY(current.targetPresent());
        QCOMPARE(retained.selectedState({"count"}).value().toInt(), 1);
        QVERIFY(retained.selectedState({"count"}).targetPresent());
    }
    void emptyDraftUsesCurrentListMetadata() {
        using D = ConfigDescriptor;
        auto row = D(D::Type::Record);
        row.fields = {{"id", D::leaf(D::Type::String, QString())}};
        const QVariantList rows{QVariantMap{{"id", "a"}}, QVariantMap{{"id", "b"}}};
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"rows", D::list(row, rows)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"rows", rows}}}},
                           true}};
        options.recordListPolicies = {{"rows", {"rows"}, {}, "id", {}, true, false}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRecordListMetadata metadata;
        metadata.ready = true;
        metadata.entries = {{"a", true, false, false, {}}};
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("rows", metadata); })
                .status,
            QString("projected"));
        auto *draft = store.draft();
        QVERIFY(draft && draft->valid());
        const auto count = resolutions(store);
        const auto retained = draft->previewProjection()->snapshot();
        QCOMPARE(retained.selectedState({"rows"}).value().toList().size(), 1);
        metadata.entries.append({"b", true, false, false, {}});
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("rows", metadata); })
                .status,
            QString("projected"));
        const auto current = draft->previewProjection()->snapshot().selectedState({"rows"});
        QCOMPARE(current.value().toList().size(), 2);
        QCOMPARE(current.candidateValue(), current.value());
        QVERIFY(current.targetPresent());
        QVERIFY(current.candidateAvailable());
        QCOMPARE(
            draft->readProjection()->snapshot().selectedState({"rows"}).value().toList().size(), 1);
        QCOMPARE(retained.selectedState({"rows"}).value().toList().size(), 1);
        QCOMPARE(resolutions(store), count);
    }
    void emptyDraftUsesPublicationCut() {
        QFETCH(int, leaves);
        using D = ConfigDescriptor;
        QMap<QString, D> fields;
        QVariantMap raw;
        for (int i = 0; i < leaves; ++i) {
            const auto key = "field" + QString::number(i);
            fields[key] = D::leaf(D::Type::SafeInteger, 1);
            raw[key] = 4;
        }
        auto absent = D::leaf(D::Type::String, QVariant());
        absent.optional = true;
        absent.defaultValue.reset();
        fields["optional"] = absent;
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"group", D::group(fields)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {"group"}, raw}},
                           true},
                          {"runtime",
                           ConfigLayerKind::Runtime,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"runtime", {"group"}, {{"field0", 8}}}},
                           false}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        QVERIFY(draft && draft->valid());
        const auto before = resolutions(store);
        QCOMPARE(before, quint64(0)); // Initial empty capture also shares its admitted cut.
        const ConfigPath leaf{"group", "field0"}, optional{"group", "optional"};
        const auto retained = draft->previewProjection()->snapshot();
        QVERIFY(retained.valid());
        QCOMPARE(retained.selectedState(leaf).value().toInt(), 8);
        QVERIFY(store.setUser(leaf, 7).accepted);
        resolutions(store);
        auto current = draft->previewProjection()->snapshot();
        QCOMPARE(current.selectedState(leaf).value().toInt(), 8);
        QCOMPARE(current.selectedState(leaf).maskedBy(), QString("runtime"));
        QVERIFY(current.selectedState(leaf).targetPresent());
        QVERIFY(current.selectedState(leaf).candidateAvailable());
        QCOMPARE(current.selectedState(leaf).candidateValue().toInt(), 8);
        QVERIFY(store.clearRuntime(leaf).accepted);
        resolutions(store);
        current = draft->previewProjection()->snapshot();
        QCOMPARE(current.selectedState(leaf).value().toInt(), 7);
        QVERIFY(current.selectedState(leaf).maskedBy().isEmpty());
        QVERIFY(store.setUser(optional, QString("present")).accepted);
        resolutions(store);
        current = draft->previewProjection()->snapshot();
        QVERIFY(current.selectedState(optional).present());
        QVERIFY(current.selectedState(optional).targetPresent());
        QCOMPARE(current.selectedState(optional).candidateValue().toString(), QString("present"));
        const auto withOptional = current;
        QVERIFY(store.reset(optional, "user").accepted);
        resolutions(store);
        current = draft->previewProjection()->snapshot();
        QVERIFY(!current.selectedState(optional).present());
        QVERIFY(!current.selectedState(optional).targetPresent());
        QVERIFY(current.selectedState(optional).candidateAvailable());
        QVERIFY(!current.selectedState(optional).candidatePresent());
        QCOMPARE(current.selectedState(leaf).pending(), ConfigSelectedState::Pending::None);
        QCOMPARE(draft->readProjection()->snapshot().selectedState(leaf).value().toInt(), 8);
        QCOMPARE(retained.selectedState(leaf).value().toInt(), 8);
        QVERIFY(!retained.selectedState(optional).present());
        QCOMPARE(withOptional.selectedState(optional).value().toString(), QString("present"));
        const auto after = resolutions(store);
        qInfo() << "EMPTY_DRAFT_PUBLICATION leaves=" << leaves
                << "extra_resolutions=" << after - before;
        QCOMPARE(after, before);
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
