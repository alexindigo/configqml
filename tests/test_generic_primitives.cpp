// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configdraft.h"
#include "configjsoncodec.h"
#include "configreadprojection.h"
#include "configpendingreply_p.h"
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
    void pendingRefusalIsQueued() {
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::refuse("unsupported-operation", "selection"));
        QSignalSpy finished(reply.get(), &ConfigPendingReply::finished);
        QVERIFY(!reply->isFinished());
        QVERIFY(finished.isEmpty());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        const auto error = reply->error();
        QCOMPARE(error.code(), QString("unsupported-operation"));
        QCOMPARE(error.phase(), QString("selection"));
        QCOMPARE(error.message(), QString("configqml: unsupported-operation"));
        reply.reset();
        QCOMPARE(error.code(), QString("unsupported-operation"));
    }
    void pendingReadComposesExactViews_data() {
        QTest::addColumn<bool>("newerInput");
        QTest::newRow("unchanged") << false;
        QTest::newRow("newer-input") << true;
    }
    void pendingReadComposesExactViews() {
        QFETCH(bool, newerInput);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})},
                                   {"b", D::group({{"count", D::leaf(T::SafeInteger, 9)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *origin = store.scope({"a"}, false)->readProjection();
        auto *first = store.readProjection();
        auto *second = store.scope({"a"}, false)->readProjection();
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        const auto inputs = store.inputs().size();
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::read(origin, {}, {first, second}));
        QSignalSpy finished(reply.get(), &ConfigPendingReply::finished);
        QVERIFY(!reply->isFinished());
        QTRY_COMPARE(held.size(), 1);
        QVERIFY(finished.isEmpty());
        ConfigStoreTestAccess::releaseReader(store, held); // Only the initiating cut.
        QTRY_COMPARE(held.size(), 2);
        QVERIFY(finished.isEmpty()); // Neither requested exact view has installed its capture.
        QCOMPARE(store.inputs().size(), inputs);
        if (newerInput)
            QVERIFY(store.setUser({"a", "count"}, 7).accepted);
        // Release the second requested view first: the reply still needs the first.
        const auto firstCapture = held.takeFirst();
        ConfigStoreTestAccess::releaseReader(store, held);
        QCoreApplication::processEvents();
        QVERIFY(!reply->isFinished());
        ConfigStoreTestAccess::inject(store, firstCapture);
        QVERIFY(!reply->isFinished()); // Terminal delivery is queued, including a no-op.
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!reply->isError());
        const auto captured = reply->value();
        QVERIFY(captured.generation().valid());
        QCOMPARE(captured.snapshot().selectedState({"count"}).value().toInt(), 3);
        const auto views = captured.observations();
        QCOMPARE(views.size(), 2);
        QCOMPARE(views[0].status(), QString("observed"));
        QCOMPARE(views[1].status(), QString("observed"));
        QCOMPARE(views[0].snapshot().selectedState({"b", "count"}).value().toInt(), 9);
        QVERIFY(!views[1].snapshot().selectedState({"b", "count"}).valid());
        QCOMPARE(views[1].snapshot().selectedState({"count"}).value().toInt(), 3);
        const bool rootCoversNewer =
            views[0].snapshot().selectedState({"a", "count"}).value().toInt() == 7;
        const QList<ConfigPath> expectedPending = newerInput && !rootCoversNewer
                                                      ? QList<ConfigPath>{{"a", "count"}}
                                                      : QList<ConfigPath>{};
        QCOMPARE(views[0].pending(), expectedPending);
        QCOMPARE(views[1].pending(),
                 newerInput ? QList<ConfigPath>{{"count"}} : QList<ConfigPath>{});
        QVERIFY(!captured.write()); // Observation is not a fabricated publication receipt.
        QProcess consumer;
        consumer.start(
            QCoreApplication::applicationFilePath(),
            {"--inert-daemon",
             QString::number(captured.snapshot().selectedState({"count"}).value().toInt())});
        QVERIFY(consumer.waitForFinished(10000));
        QCOMPARE(consumer.exitCode(), 0);
        QCOMPARE(consumer.readAllStandardOutput().trimmed(), QByteArray("DAEMON_ARGV 3"));
        // Late duplicate captures cannot finish again or mutate detached results.
        ConfigStoreTestAccess::inject(store, firstCapture);
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 1);
        reply.reset();
        QCOMPARE(captured.snapshot().selectedState({"count"}).value().toInt(), 3);
    }
    void pendingViewSelectionRefusesBeforeCapture() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})},
                                   {"b", D::group({{"count", D::leaf(T::SafeInteger, 9)}})}}));
        ConfigStore store(options), unrelated(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(unrelated.status(), ConfigStore::Status::Ready);
        auto *root = store.readProjection();
        auto *narrow = store.scope({"a"}, false)->readProjection();
        QSignalSpy reads(root, &ConfigReadProjection::completed);
        for (auto *view : {narrow, unrelated.readProjection()}) {
            std::unique_ptr<ConfigPendingReply> reply(
                ConfigPendingReplyState::read(root, {}, {view}));
            QVERIFY(!reply->isFinished());
            QTRY_VERIFY(reply->isFinished());
            QVERIFY(reply->isError());
            QCOMPARE(reply->error().code(), QString("read-observation"));
            QCOMPARE(reply->error().phase(), QString("selection"));
            QCOMPARE(reply->value().observations().size(), 1);
            QCOMPARE(reply->value().observations()[0].status(), QString("not_observed"));
        }
        QVERIFY(reads.isEmpty());
        QVERIFY(store.inputs().isEmpty());
    }
    void pendingReaderFailureRetainsInitiatingCapture() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath("pending.json");
        const QByteArray bytes("{\"a\":{\"count\":3}}");
        compat::put(file, bytes);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 1)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        options.jsonFiles = {{"user", "user", file}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *origin = store.scope({"a"}, false)->readProjection();
        auto *reader = store.readProjection();
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::read(origin, {}, {reader}));
        QTRY_COMPARE(held.size(), 1);
        const auto lastGood = reader->observation();
        compat::put(file, "{");
        ConfigStoreTestAccess::releaseReader(store, held);
        QTRY_COMPARE(held.size(), 1);
        QVERIFY(!reply->isFinished());
        ConfigStoreTestAccess::releaseReader(store, held);
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        QCOMPARE(reply->error().phase(), QString("observation"));
        QCOMPARE(reply->error().code(), QString("read-acquisition"));
        const auto captured = reply->value();
        QVERIFY(captured.generation().valid());
        QCOMPARE(captured.snapshot().selectedState({"count"}).value().toInt(), 3);
        QVERIFY(!captured.write());
        QCOMPARE(reader->observation(), lastGood);
        QCOMPARE(captured.observations()[0].status(), QString("failed"));
        compat::put(file, bytes);
        std::unique_ptr<ConfigPendingReply> recovery(
            ConfigPendingReplyState::read(reader, captured.generation()));
        QTRY_COMPARE(held.size(), 1);
        ConfigStoreTestAccess::releaseReader(store, held);
        QTRY_VERIFY(recovery->isFinished());
        QVERIFY(!recovery->isError());
        QCOMPARE(recovery->value().snapshot().selectedState({"a", "count"}).value().toInt(), 3);
        QVERIFY(store.inputs().isEmpty());
        QCOMPARE(compat::bytes(file), bytes);
    }
    void pendingReplySurvivesPresentationLoss() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *scope = store.scope({"a"}, false);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::read(scope->readProjection()));
        QTRY_COMPARE(held.size(), 1);
        delete scope;
        QVERIFY(!reply->isFinished());
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        QCOMPARE(reply->error().phase(), QString("lifetime"));
        ConfigStoreTestAccess::releaseReader(store, held);
        // Consumer deletion during the one terminal signal has no continuation.
        auto *destroyed = ConfigPendingReplyState::refuse("unsupported-operation", "selection");
        QPointer<ConfigPendingReply> guard(destroyed);
        connect(destroyed, &ConfigPendingReply::finished, this, [destroyed] { delete destroyed; });
        QTRY_VERIFY(!guard);
    }
    void pendingReadNotificationFence() {
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
        bool started = false, inside = false, early = false;
        std::unique_ptr<ConfigPendingReply> reply;
        connect(&store, &ConfigStore::valuesChanged, this, [&] {
            if (started)
                return;
            started = inside = true;
            reply.reset(ConfigPendingReplyState::read(store.readProjection()));
            connect(reply.get(), &ConfigPendingReply::finished, this, [&] { early |= inside; });
            QEventLoop nested;
            ConfigStoreTestAccess::run(
                store, [](ConfigWorker &) {}, &nested, [&] { nested.quit(); });
            nested.exec();
            early |= reply->isFinished();
            inside = false;
        });
        QVERIFY(store.setUser({"count"}, 3).accepted);
        QTRY_VERIFY(reply && reply->isFinished());
        QVERIFY(!early);
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().snapshot().selectedState({"count"}).value().toInt(), 3);
    }
    void explicitCandidateTargets_data() {
        QTest::addColumn<bool>("runtime");
        QTest::newRow("user") << false;
        QTest::newRow("runtime") << true;
    }
    void explicitCandidateTargets() {
        QFETCH(bool, runtime);
        const ConfigSchema schema(D::group({{"a", D::leaf(T::SafeInteger, 1)},
                                            {"b", D::leaf(T::SafeInteger, 2)},
                                            {"c", D::leaf(T::SafeInteger, 3)}}));
        ConfigLayers base(
            schema, {{"user",
                      ConfigLayerKind::File,
                      ConfigAdmissionPolicy::FieldFallback,
                      {{"user", {}, {{"a", 4}, {"b", 5}, {"unknown", "user-owned"}}}},
                      true},
                     {"runtime",
                      ConfigLayerKind::Runtime,
                      ConfigAdmissionPolicy::FieldFallback,
                      {{"runtime", {}, {{"a", 8}, {"c", 9}, {"unknown", "runtime-owned"}}}}}});
        const auto target =
            runtime ? ConfigSelection::Target::RuntimeLayer : ConfigSelection::Target::UserLayer;
        const auto selection =
            ConfigSelection::capture(schema, {{}}, schema.leaves(), target, "edit-selection");
        auto batch = ConfigEditBatch::capture(selection, {{{"a"}, std::nullopt}, {{"b"}, 6}});
        batch.sequence = 17;
        QList<ConfigInput> admitted;
        const auto candidate = resolveCandidate(base, batch, {}, {}, [&](const ConfigInput &input) {
            admitted.append(input);
            return QString();
        });
        QVERIFY2(candidate.valid(), qPrintable(candidate.error));
        QCOMPARE(admitted.size(), 2);
        QCOMPARE(admitted[0].kind,
                 runtime ? ConfigInput::Kind::ClearRuntime : ConfigInput::Kind::Reset);
        QCOMPARE(admitted[1].kind, runtime ? ConfigInput::Kind::Runtime : ConfigInput::Kind::User);
        QCOMPARE(admitted[0].sequence, quint64(17));
        const auto a = candidate.facts.value({"a"}), b = candidate.facts.value({"b"});
        QVERIFY(a.targetPresent);
        QCOMPARE(b.targetPresent, !runtime);
        QCOMPARE(a.effective.toInt(), runtime ? 4 : 8);
        QCOMPARE(b.effective.toInt(), 6);
        QCOMPARE(a.maskedBy, runtime ? QString() : QString("runtime"));
        QCOMPARE(candidate.facts.value({"c"}).targetPresent, runtime);
        QVERIFY(a.edited && b.edited);
        QVERIFY(!candidate.facts.value({"c"}).edited);
        const QString targetId = runtime ? "runtime" : "user";
        auto expected = base.source(targetId, targetId)->raw;
        expected.remove("a");
        expected["b"] = qint64(6);
        QCOMPARE(candidate.layers->source(targetId, targetId)->raw, expected);
        const QString otherId = runtime ? "user" : "runtime";
        QCOMPARE(candidate.layers->source(otherId, otherId)->raw,
                 base.source(otherId, otherId)->raw);
        QCOMPARE(base.value({"a"})->toInt(), 8);
        QCOMPARE(base.value({"b"})->toInt(), 5);
        QCOMPARE(base.info({"a"}).intentRevision, quint64(0));
        QVERIFY(base.takeProjectionEffects().isEmpty());
    }
    void runtimeCandidateRefusalIsAtomic() {
        const ConfigSchema schema(
            D::group({{"a", D::leaf(T::SafeInteger, 1)}, {"b", D::leaf(T::SafeInteger, 2)}}));
        ConfigSource source{"runtime", {}, {{"a", 4}, {"b", 5}}};
        source.unknownData = ConfigUnknownData::Strict;
        ConfigLayers base(schema, {{"runtime",
                                    ConfigLayerKind::Runtime,
                                    ConfigAdmissionPolicy::FieldFallback,
                                    {source}}});
        const auto selection = ConfigSelection::capture(
            schema, {{}}, schema.leaves(), ConfigSelection::Target::RuntimeLayer, "edit-selection");
        const auto batch = ConfigEditBatch::capture(selection, {{{"a"}, 6}, {{"b"}, 7}});
        int checked = 0;
        const auto refused = resolveCandidate(base, batch, {}, {}, [&](const ConfigInput &input) {
            ++checked;
            return input.path == ConfigPath{"b"} ? QString("policy-refused") : QString();
        });
        QCOMPARE(checked, 2);
        QCOMPARE(refused.error, QString("policy-refused"));
        QVERIFY(!refused.layers);
        QCOMPARE(base.source("runtime", "runtime")->raw, source.raw);
        QCOMPARE(base.info({"a"}).intentRevision, quint64(0));
        const auto invalid =
            ConfigEditBatch::capture(selection, {{{"a"}, 6}, {{"b"}, "wrong-type"}});
        QVERIFY(!invalid.error.isEmpty());
        checked = 0;
        QVERIFY(!resolveCandidate(base, invalid, {}, {}, [&](const ConfigInput &) {
                     ++checked;
                     return QString();
                 }).valid());
        QCOMPARE(checked, 0);
        const ConfigLayers missing(schema, {});
        QCOMPARE(resolveCandidate(missing, batch, {}, {}).error, QString("no-write-target"));
        const auto removed = ConfigEditBatch::capture(selection, {{{"a"}, std::nullopt}});
        QCOMPARE(resolveCandidate(missing, removed, {}, {}).error, QString("no-runtime-target"));
    }
    void candidateProjectsWholeBatch_data() {
        QTest::addColumn<bool>("runtime");
        QTest::addColumn<int>("b");
        QTest::newRow("user-complete-candidate") << false << 2;
        QTest::newRow("user-invalid-final-candidate") << false << 3;
        QTest::newRow("runtime-complete-candidate") << true << 2;
        QTest::newRow("runtime-keeps-file-projection-input") << true << 3;
    }
    void candidateProjectsWholeBatch() {
        QFETCH(bool, runtime);
        QFETCH(int, b);
        auto integer = D::leaf(T::SafeInteger, 1);
        integer.admissionPolicy = D::AdmissionPolicy::Strict;
        const ConfigSchema schema(D::group({{"a", integer}, {"b", integer}}));
        int projections = 0;
        ConfigProjectionSpec projection{
            "pair", {"user"}, {}, [&](const ConfigProjectionInput &input) {
                ++projections;
                auto values = input.values;
                if (input.raw.value("a") != input.raw.value("b"))
                    values["a"] = false;
                return ConfigProjectionResult{values, input.compatibilityRaw, {}, {}};
            }};
        const QVariantMap raw{{"a", 1}, {"b", 1}};
        ConfigLayers base(schema,
                          {{"user",
                            ConfigLayerKind::File,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"user", {}, raw}},
                            true},
                           {"runtime",
                            ConfigLayerKind::Runtime,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"runtime", {}, raw}}}},
                          {projection});
        QVERIFY(base.valid());
        QVERIFY(!base.takeProjectionEffects().isEmpty()); // Initial source acquisition event.
        projections = 0;
        const auto selection = ConfigSelection::capture(
            schema, {{}}, schema.leaves(),
            runtime ? ConfigSelection::Target::RuntimeLayer : ConfigSelection::Target::UserLayer,
            "edit-selection");
        const auto candidate = resolveCandidate(
            base, ConfigEditBatch::capture(selection, {{{"a"}, 2}, {{"b"}, b}}), {}, {});
        QCOMPARE(projections, 1); // Never evaluate the inconsistent intermediate a=2,b=1.
        QCOMPARE(candidate.valid(), runtime || b == 2);
        if (candidate.valid()) {
            QCOMPARE(candidate.facts.value({"a"}).effective.toInt(), runtime ? 2 : 1);
            QCOMPARE(candidate.facts.value({"b"}).effective.toInt(), runtime ? b : 1);
        } else {
            QVERIFY(!candidate.error.isEmpty());
            QVERIFY(!candidate.layers);
        }
        QCOMPARE(base.source("runtime", "runtime")->raw, raw);
        QCOMPARE(base.source("user", "user")->raw, raw);
        QCOMPARE(base.info({"a"}).intentRevision, quint64(0));
        QVERIFY(base.takeProjectionEffects().isEmpty());
    }
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
