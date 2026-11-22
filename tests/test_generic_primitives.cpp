// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configdraft.h"
#include "configjsoncodec.h"
#include "configreadprojection.h"
#include "configstorehandle.h"
#include "storeprobe.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QProcess>
#include <cstdio>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;

class GenericPrimitivesTest : public QObject {
    Q_OBJECT
private slots:
    void mixedDraftPending() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"a", D::leaf(T::SafeInteger, 1)}, {"b", D::leaf(T::SafeInteger, 2)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"a", 4}, {"b", 5}}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        auto *projection = draft->readProjection();
        QCOMPARE(compat::request(store, [&] { return draft->stageReset({{}}); }).status,
                 QString("staged"));
        QCOMPARE(projection->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(draft->setValue({"a"}, 8).accepted);
        QCOMPARE(projection->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Set);
        QVERIFY(!projection->selectedState({"a"}).candidateAvailable());
        QVERIFY(!projection->selectedState({"b"}).candidateAvailable());
        QTRY_VERIFY(projection->selectedState({"a"}).candidateAvailable());
        QCOMPARE(projection->selectedState({"a"}).candidateValue().toInt(), 8);
        QCOMPARE(projection->selectedState({"b"}).candidateValue().toInt(), 2);
        QCOMPARE(projection->selectedState({"b"}).pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(store.inputs().isEmpty());
        QVERIFY(draft->discard());
        QCOMPARE(store.value({"a"}).toInt(), 4);
    }
    void closedPreviewRejectsDelayedCapture() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        QVERIFY(draft->setValue({"count"}, 7).accepted);
        auto *preview = draft->previewProjection();
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        bool finished = false;
        preview->synchronize({}, this, [&](const ConfigReadResult &result) {
            finished = true;
            QCOMPARE(result.status(), QString("failed"));
        });
        QEventLoop loop;
        onHeld = [&] { loop.quit(); };
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(10000);
        loop.exec();
        onHeld = {};
        QVERIFY(!held.isEmpty());
        QVERIFY(!finished);
        const auto captured = held.first().reads.first().snapshot;
        QCOMPARE(captured.selectedState({"count"}).value().toInt(), 7);
        QVERIFY(draft->discard());
        QVERIFY(finished);
        QCOMPARE(preview->snapshot().selectedState({"count"}).value().toInt(), 4);
        QVERIFY(!captured.observation().valid());
        ConfigStoreTestAccess::releaseReader(store, held);
        QCOMPARE(preview->snapshot().selectedState({"count"}).value().toInt(), 4);
        QCOMPARE(captured.selectedState({"count"}).value().toInt(), 7);
    }
    void notificationFence() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *projection = store.readProjection();
        bool started = false, inside = false, called = false, early = false;
        connect(&store, &ConfigStore::valuesChanged, this, [&] {
            if (started)
                return;
            started = inside = true;
            projection->synchronize({}, this, [&](const ConfigReadResult &result) {
                early |= inside;
                called = true;
                QCOMPARE(result.status(), QString("observed"));
                QCOMPARE(result.snapshot().selectedState({"count"}).value().toInt(), 3);
            });
            QEventLoop nested;
            ConfigStoreTestAccess::run(
                store, [](ConfigWorker &) {}, &nested, [&] { nested.quit(); });
            nested.exec(); // Reader's worker command finished during the notification pass.
            early |= called;
            inside = false;
        });
        QVERIFY(store.setUser({"count"}, 3).accepted);
        QTRY_VERIFY(called);
        QVERIFY(!early);
    }
    void ordinaryDraftCandidate() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 1)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"count", 4}}}},
                           true},
                          {"runtime",
                           ConfigLayerKind::Runtime,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"runtime", {}, {{"count", 9}}}},
                           false}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        auto *projection = draft->readProjection(); // Provisional slice-facing capability.
        QSignalSpy completed(&store, &ConfigStore::completed);
        const auto inputs = store.inputs().size();
        QVERIFY(draft->setValue({"count"}, 7).accepted);
        QVERIFY(!draft->busy());
        QCOMPARE(projection->selectedState({"count"}).pending(), ConfigSelectedState::Pending::Set);
        QVERIFY(!projection->selectedState({"count"}).candidateAvailable());
        QVERIFY(draft->setValue({"count"}, 8).accepted);
        QTRY_VERIFY(projection->selectedState({"count"}).candidateAvailable());
        const auto captured = projection->snapshot();
        const auto state = captured.selectedState({"count"});
        QCOMPARE(state.authoredValue().toInt(), 8);
        QCOMPARE(state.value().toInt(), 9);
        QCOMPARE(state.candidateValue().toInt(), 9);
        QVERIFY(state.targetPresent());
        QCOMPARE(state.maskedBy(), QString("runtime"));
        QVERIFY(completed.isEmpty()); // Setters add no public operation completions.
        QCOMPARE(store.inputs().size(), inputs);
        QVERIFY(draft->setValue({"count"}, 6).accepted);
        QVERIFY(!draft->busy());
        const auto applied = compat::request(store, [&] { return draft->apply(); });
        QCOMPARE(applied.status, QString("saved")); // No new candidate-busy gate.
        QCOMPARE(captured.selectedState({"count"}).authoredValue().toInt(), 8);
        QCOMPARE(store.value({"count"}).toInt(), 9);
    }
    void selectedCancellableReset_data() {
        QTest::addColumn<QByteArray>("original");
        QTest::addColumn<int>("inheritedCount");
        QTest::addColumn<int>("inheritedHidden");
        QTest::newRow("override") << QByteArray("{\"count\":4,\"hidden\":5}") << 3 << 8;
        QTest::newRow("equal-override") << QByteArray("{\"count\":3,\"hidden\":8}") << 3 << 8;
        QTest::newRow("no-override") << QByteArray("{}") << 3 << 8;
        QTest::newRow("unknown-sibling")
            << QByteArray("{\"count\":4,\"hidden\":5,\"opaque\":18446744073709551615}") << 1 << 2;
        QTest::newRow("unknown-only") << QByteArray("{\"opaque\":true}") << 1 << 2;
    }
    void selectedCancellableReset() {
        QFETCH(QByteArray, original);
        QFETCH(int, inheritedCount);
        QFETCH(int, inheritedHidden);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath("configuration.json");
        const auto protectedFile = directory.filePath("content.json");
        compat::put(file, original);
        const QByteArray protectedBytes("{\"protected\":99,\"opaque\":18446744073709551615}");
        compat::put(protectedFile, protectedBytes);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"configuration", D::group({{"count", D::leaf(T::SafeInteger, 1)},
                                                  {"hidden", D::leaf(T::SafeInteger, 2)}})},
                      {"content", D::group({{"protected", D::leaf(T::SafeInteger, 9)}})}}));
        ConfigSource legacy{"legacy", {"configuration"}, {{"count", 3}, {"hidden", 8}}};
        legacy.fallbackFor = "configuration";
        options.layers = {{"legacy",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {legacy},
                           false},
                          {"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"configuration", {"configuration"}, {}}, {"content", {"content"}, {}}},
                           true}};
        options.jsonFiles = {{"user", "configuration", file}, {"user", "content", protectedFile}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"configuration"}});
        auto *projection = draft->readProjection();
        ConfigObservation stagedObservation;
        connect(projection, &ConfigReadProjection::operationCompleted, this,
                [&](const ConfigResult &result, const ConfigObservation &observation) {
                    if (result.operation == "stageReset")
                        stagedObservation = observation;
                });
        QVERIFY(draft->setValue({"configuration", "count"}, 12).accepted); // Pending wizard input.
        const auto inputs = store.inputs().size();
        const auto intent = store.info({"configuration", "count"}).intentRevision;
        QCOMPARE(
            compat::request(store, [&] { return draft->stageReset({{"configuration"}}); }).status,
            QString("staged"));
        QVERIFY(stagedObservation.valid());
        bool rejectedStage = false;
        store.readProjection()->synchronize(
            stagedObservation, this, [&](const ConfigReadResult &result) {
                rejectedStage = true;
                QCOMPARE(result.status(), QString("failed"));
                QCOMPARE(result.error().code, QString("read-observation"));
            });
        QTRY_VERIFY(rejectedStage); // Staged coverage never certifies committed live state.
        const auto count = projection->selectedState({"configuration", "count"});
        QCOMPARE(count.pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(count.candidateAvailable());
        QCOMPARE(count.value().toInt(), inheritedCount);
        const auto originalRaw = decodeJsonValue(original).value.toMap();
        QCOMPARE(count.targetPresent(), originalRaw.contains("count"));
        QCOMPARE(projection->selectedState({"configuration", "hidden"}).value().toInt(),
                 inheritedHidden);
        QVERIFY(!projection->selectedState({"content", "protected"}).valid());
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info({"configuration", "count"}).intentRevision, intent);
        QCOMPARE(compat::bytes(file), original);
        QCOMPARE(compat::bytes(protectedFile), protectedBytes);
        QVERIFY(draft->discard());
        QCOMPARE(compat::bytes(file), original);
        QVERIFY(!draft->dirty());
        QCOMPARE(projection->selectedState({"configuration", "count"}).pending(),
                 ConfigSelectedState::Pending::None);
        QCOMPARE(
            compat::request(store, [&] { return draft->stageReset({{"configuration"}}); }).status,
            QString("staged"));
        const auto applied = compat::request(store, [&] { return draft->apply(); });
        QVERIFY(applied.status == "saved" || applied.status == "nothing_to_save");
        const auto decoded = decodeJsonValue(compat::bytes(file));
        QVERIFY(decoded.ok());
        QVERIFY(!decoded.value.toMap().contains("count"));
        QVERIFY(!decoded.value.toMap().contains("hidden"));
        QCOMPARE(decoded.value.toMap().value("opaque"), originalRaw.value("opaque"));
        QCOMPARE(compat::bytes(protectedFile), protectedBytes);
        ConfigStore cold(options);
        QTRY_COMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(cold.value({"configuration", "count"}).toInt(), inheritedCount);
        QCOMPARE(cold.value({"configuration", "hidden"}).toInt(), inheritedHidden);
        QCOMPARE(cold.value({"content", "protected"}).toInt(), 99);
    }
    void orderedStoreRead_data() {
        QTest::addColumn<int>("value");
        QTest::newRow("changed") << 3;
        QTest::newRow("equal") << 4;
    }
    void orderedStoreRead() {
        QFETCH(int, value);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *read = store.readProjection();
        ConfigObservation write;
        connect(read, &ConfigReadProjection::operationCompleted, this,
                [&](const ConfigResult &result, const ConfigObservation &observation) {
                    if (result.operation == "save")
                        write = observation;
                });
        QCOMPARE(compat::request(store, [&] { return store.setUser({"count"}, value).requestId; })
                     .status,
                 QString("accepted"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(write.valid());
        bool signalled = false, called = false;
        connect(read, &ConfigReadProjection::completed, this, [&](const ConfigReadResult &result) {
            signalled = true;
            QCOMPARE(result.status(), QString("observed"));
            QCOMPARE(result.snapshot().selectedState({"count"}).value().toInt(), value);
        });
        read->synchronize(write, this, [&](const ConfigReadResult &result) {
            QVERIFY(signalled);
            QCOMPARE(result.status(), QString("observed"));
            QCOMPARE(store.value({"count"}).toInt(), value);
            called = true;
        });
        QVERIFY(!called);
        QTRY_VERIFY(called);
        const auto snapshot = read->snapshot();
        QCOMPARE(
            compat::request(store, [&] { return store.setUser({"count"}, 7).requestId; }).status,
            QString("accepted"));
        QCOMPARE(snapshot.selectedState({"count"}).value().toInt(), value);
    }
    void heldRuntimeRead_data() {
        QTest::addColumn<int>("value");
        QTest::addColumn<bool>("ownerFirst");
        QTest::addColumn<bool>("edit");
        QTest::addColumn<bool>("token");
        QTest::addColumn<bool>("newerInput");
        QTest::addColumn<bool>("acquisitionFailure");
        for (bool ownerFirst : {false, true}) {
            const auto suffix = ownerFirst ? "-owner-first" : "-base-first";
            QTest::newRow(qPrintable(QString("changed") + suffix))
                << 3 << ownerFirst << true << true << false << false;
            QTest::newRow(qPrintable(QString("equal") + suffix))
                << 4 << ownerFirst << true << true << false << false;
            QTest::newRow(qPrintable(QString("no-edit") + suffix))
                << 4 << ownerFirst << false << true << false << false;
            QTest::newRow(qPrintable(QString("no-token") + suffix))
                << 3 << ownerFirst << true << false << false << false;
            QTest::newRow(qPrintable(QString("newer-input") + suffix))
                << 3 << ownerFirst << true << true << true << false;
            QTest::newRow(qPrintable(QString("read-failure") + suffix))
                << 3 << ownerFirst << true << true << false << true;
        }
    }
    void heldRuntimeRead() {
        QFETCH(int, value);
        QFETCH(bool, ownerFirst);
        QFETCH(bool, edit);
        QFETCH(bool, token);
        QFETCH(bool, newerInput);
        QFETCH(bool, acquisitionFailure);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto ownerFile = directory.filePath("owner.json");
        compat::put(ownerFile, "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"count\":4}}]}");
        ConfigStoreOptions baseOptions;
        baseOptions.autoSave = false;
        baseOptions.schema = ConfigSchema(
            D::group({{"settings", D::group({{"count", D::leaf(T::SafeInteger, 1)}})}}));
        auto row = D(T::Record);
        row.fields = {{"id", D::leaf(T::String, QString())}};
        ConfigStoreOptions ownerOptions;
        ownerOptions.autoSave = false;
        ownerOptions.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        ownerOptions.layers = {
            {"user",
             ConfigLayerKind::File,
             ConfigAdmissionPolicy::FieldFallback,
             {{"user",
               {},
               {{"rows", QVariantList{QVariantMap{
                             {"id", "x"}, {"instanceSettings", QVariantMap{{"count", 4}}}}}}}}},
             true}};
        ownerOptions.jsonFiles = {{"user", "user", ownerFile}};
        std::unique_ptr<ConfigStore> baseOwner, slotOwner;
        if (ownerFirst) {
            slotOwner = std::make_unique<ConfigStore>(ownerOptions);
            baseOwner = std::make_unique<ConfigStore>(baseOptions);
        } else {
            baseOwner = std::make_unique<ConfigStore>(baseOptions);
            slotOwner = std::make_unique<ConfigStore>(ownerOptions);
        }
        auto &base = *baseOwner;
        auto &owner = *slotOwner;
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        const auto baseOrder =
            ConfigObservationAccess::dependencies(base.readProjection()->observation())
                .first()
                .incarnation;
        const auto ownerOrder =
            ConfigObservationAccess::dependencies(owner.readProjection()->observation())
                .first()
                .incarnation;
        QCOMPARE(ownerOrder < baseOrder, ownerFirst);
        ConfigOverlayOptions options;
        options.base = base.scope({"settings"}, false);
        options.ownerPath = {"rows"};
        options.ownerGeneration = owner.info({"rows"}).publicationGeneration;
        options.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
        options.identityPath = {{ConfigValueSelector::Kind::Key, "id", 0}};
        options.identityValue = "x";
        options.overrideKey = "instanceSettings";
        options.mode = ConfigOverlayMode::Live;
        const auto runtimeResult =
            compat::request(owner, [&] { return owner.openOverlay(options); });
        QCOMPARE(runtimeResult.status, QString("opened"));
        auto *runtime = ConfigStoreTestAccess::overlay(owner, runtimeResult.editorId);
        QVERIFY(runtime);
        options.mode = ConfigOverlayMode::Editor;
        const auto writerResult =
            compat::request(owner, [&] { return owner.openOverlay(options); });
        QCOMPARE(writerResult.status, QString("opened"));
        auto *writer = ConfigStoreTestAccess::overlay(owner, writerResult.editorId);
        QVERIFY(writer);
        auto *writerProjection = writer->readProjection();
        QSignalSpy candidates(writerProjection, &ConfigReadProjection::stateChanged);
        const auto inputCount = owner.inputs().size();
        const auto originalOwner = owner.value({"rows"});
        QVERIFY(writer->unset({"count"}));
        QVERIFY(!writer->value({"count"}).isValid()); // Released editor-unset behavior.
        QCOMPARE(writerProjection->selectedState({"count"}).pending(),
                 ConfigSelectedState::Pending::Remove);
        QVERIFY(!writerProjection->selectedState({"count"}).candidateAvailable());
        QTRY_VERIFY(!candidates.isEmpty());
        const auto reset = writerProjection->selectedState({"count"});
        QVERIFY(reset.candidateAvailable());
        QVERIFY(reset.targetPresent());
        QCOMPARE(reset.candidateValue().toInt(), 1);
        QCOMPARE(reset.value().toInt(), 1);
        QCOMPARE(owner.inputs().size(), inputCount);
        QCOMPARE(owner.value({"rows"}), originalOwner); // Includes exact raw numeric atoms.
        QCOMPARE(compat::request(owner, [&] { return writer->discard(); }).status,
                 QString("discarded"));
        QCOMPARE(writer->value({"count"}).toInt(), 4);
        QCOMPARE(writerProjection->selectedState({"count"}).pending(),
                 ConfigSelectedState::Pending::None);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReader(owner, runtime->editorId(), &scheduler, held, onHeld);
        ConfigObservation write;
        connect(writer->readProjection(), &ConfigReadProjection::operationCompleted, this,
                [&](const ConfigResult &result, const ConfigObservation &token) {
                    if (result.operation == "overlayApply")
                        write = token;
                });
        if (edit)
            QVERIFY(writer->setValue({"count"}, value).accepted);
        const auto saved = compat::request(owner, [&] { return writer->apply(); });
        QVERIFY(saved.status == "saved" || saved.status == "nothing_to_save");
        QCOMPARE(saved.refreshState, QString("current"));
        QVERIFY(write.valid());
        QCOMPARE(runtime->value({"count"}).toInt(), 4);
        bool called = false;
        ConfigReadResult observed;
        int readSignals = 0, callbacks = 0;
        connect(runtime->readProjection(), &ConfigReadProjection::completed, this,
                [&](const ConfigReadResult &result) {
                    ++readSignals;
                    if (result.status() == "observed")
                        QCOMPARE(runtime->value({"count"}).toInt(), newerInput ? 7 : value);
                });
        const auto synchronize = [&] {
            runtime->readProjection()->synchronize(
                token ? write : ConfigObservation(), this, [&](const ConfigReadResult &result) {
                    QCOMPARE(readSignals, callbacks + 1);
                    ++callbacks;
                    if (result.status() == "observed")
                        QCOMPARE(runtime->value({"count"}).toInt(), newerInput ? 7 : value);
                    observed = result;
                    called = true;
                });
        };
        if (acquisitionFailure) {
            const auto lastGood = runtime->readProjection()->observation();
            const auto goodBytes = compat::bytes(ownerFile);
            compat::put(ownerFile, "{");
            synchronize();
            QTRY_VERIFY(called);
            QCOMPARE(observed.status(), QString("failed"));
            QCOMPARE(runtime->readProjection()->observation(), lastGood);
            QCOMPARE(runtime->value({"count"}).toInt(), 4);
            called = false;
            compat::put(ownerFile, goodBytes);
        }
        synchronize();
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        onHeld = [&] {
            for (const auto &packet : held)
                for (const auto &p : packet.overlays)
                    if (p.read.requestId)
                        loop.quit();
        };
        deadline.start(10000);
        loop.exec();
        onHeld = {};
        bool exact = false;
        for (const auto &packet : held)
            for (const auto &p : packet.overlays)
                exact |= p.read.requestId != 0;
        QVERIFY(exact);
        if (called) {
            QProcess premature;
            premature.start(
                QCoreApplication::applicationFilePath(),
                {"--inert-daemon",
                 QString::number(observed.snapshot().selectedState({"count"}).value().toInt())});
            QVERIFY(premature.waitForFinished(10000));
            qInfo().noquote() << "GENERIC_BARRIER_BYPASS"
                              << premature.readAllStandardOutput().trimmed();
        }
        QVERIFY(!called);
        if (newerInput)
            QVERIFY(runtime->setValue({"count"}, 7).accepted);
        delete writer; // Committed coverage survives editor destruction.
        QVERIFY(write.valid());
        const auto older = held;
        ConfigStoreTestAccess::releaseReader(owner, held);
        QVERIFY(called);
        QCOMPARE(observed.status(), QString("observed"));
        QCOMPARE(runtime->value({"count"}).toInt(), newerInput ? 7 : value);
        QCOMPARE(observed.snapshot().selectedState({"count"}).value().toInt(), value);
        QCOMPARE(observed.pending(),
                 newerInput ? QList<ConfigPath>{{"count"}} : QList<ConfigPath>{});
        QProcess consumer;
        consumer.start(
            QCoreApplication::applicationFilePath(),
            {"--inert-daemon",
             QString::number(observed.snapshot().selectedState({"count"}).value().toInt())});
        QVERIFY(consumer.waitForFinished(10000));
        QCOMPARE(consumer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(consumer.exitCode(), 0);
        QCOMPARE(consumer.readAllStandardOutput().trimmed(),
                 "DAEMON_ARGV " + QByteArray::number(value));
        QCOMPARE(compat::request(owner, [&] { return runtime->setValue({"count"}, 7).requestId; })
                     .status,
                 QString("accepted"));
        ConfigStoreTestAccess::releaseReader(owner, held);
        QCOMPARE(runtime->value({"count"}).toInt(), 7);
        for (const auto &packet : older)
            ConfigStoreTestAccess::inject(owner, packet);
        QCOMPARE(runtime->value({"count"}).toInt(), 7);
        QCOMPARE(runtime->readProjection()->snapshot().selectedState({"count"}).value().toInt(), 7);
    }
    void removalCandidateUsesPublicationProjection() {
        auto row = D(T::Record);
        row.fields = {{"id", D::leaf(T::String, QString())}};
        const QVariantList inherited{QVariantMap{{"id", "visible"}},
                                     QVariantMap{{"id", "hidden"}, {"opaque", true}}};
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"rows", D::list(row, inherited)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"rows", QVariantList{QVariantMap{{"id", "visible"}}}}}}},
                           true}};
        options.recordListPolicies = {{"rows", {"rows"}, {}, "id", {}, true, false}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRecordListMetadata metadata;
        metadata.ready = true;
        metadata.entries = {{"visible", true, false, false, {}}};
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("rows", metadata); })
                .status,
            QString("projected"));
        auto *draft = store.draft({{"rows"}});
        QVERIFY(draft->valid());
        const auto inputs = store.inputs().size();
        const auto intent = store.info({"rows"}).intentRevision;
        QSignalSpy effects(&store, &ConfigStore::projectionEffects);
        QCOMPARE(compat::request(store, [&] { return draft->stageReset({{"rows"}}); }).status,
                 QString("staged"));
        const auto candidate = draft->previewValue({"rows"});
        QCOMPARE(candidate.toList().size(), 1);
        QCOMPARE(candidate.toList().first().toMap().value("id").toString(), QString("visible"));
        QCOMPARE(draft->value({"rows"}), candidate);
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info({"rows"}).intentRevision, intent);
        QVERIFY(effects.isEmpty());
        QVERIFY(draft->discard());
        QCOMPARE(compat::request(store, [&] { return draft->stageReset({{"rows"}}); }).status,
                 QString("staged"));
        QCOMPARE(compat::request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.value({"rows"}), candidate);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--inert-daemon") {
        if (app.arguments().size() != 3)
            return 2;
        std::printf("DAEMON_ARGV %s\n", qPrintable(app.arguments()[2]));
        return 0;
    }
    GenericPrimitivesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_generic_primitives.moc"
