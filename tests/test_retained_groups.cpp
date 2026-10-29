// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "storeprobe.h"
#include "confignumber.h"
#include "configdraft.h"
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QSemaphore>
#include <atomic>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
ConfigStoreOptions options(const QString &directory = {}) {
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(
        D::group({{"choice", D::leaf(T::SafeInteger, 0)}, {"other", D::leaf(T::SafeInteger, 0)}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"user", {}, {}}},
                 true}};
    if (!directory.isEmpty())
        o.jsonFiles = {{"user", "user", directory + "/user.json"}};
    return o;
}
ConfigRetainedGroupOptions choice(const QString &key, int value,
                                  const QStringList &dependencies = {}) {
    return {key, {{"choice"}}, {{{"choice"}, true, value}}, dependencies};
}
int disk(const QString &path, const QString &key) {
    ConfigJsonCodec codec;
    const auto raw = codec.decode(compat::bytes(path), "user");
    if (!raw.ok())
        qFatal("decode fixture");
    return D::leaf(T::SafeInteger, 0).validate(raw.raw.value(key)).value.toInt();
}
void control(ConfigStore &store, const std::function<void(ConfigWorker &)> &action) {
    QEventLoop loop;
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    bool complete = false;
    ConfigStoreTestAccess::run(store, action, &loop, [&] {
        complete = true;
        loop.quit();
    });
    loop.exec();
    if (!complete)
        qFatal("worker control timeout");
}
} // namespace
class RetainedGroupsTest : public QObject {
    Q_OBJECT
private slots:
    void lifecycleRequestsCaptureSubmissionGeneration_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("success");
        QTest::newRow("gate-busy-retry-fails") << QString("retry") << false;
        QTest::newRow("release-during-failed-retry") << QString("release") << false;
        QTest::newRow("release-during-successful-retry") << QString("release") << true;
        QTest::newRow("retain-during-failed-retry") << QString("retain") << false;
        QTest::newRow("retain-during-successful-retry") << QString("retain") << true;
        QTest::newRow("reservation-during-failed-retry") << QString("reserve") << false;
        QTest::newRow("reservation-during-successful-retry") << QString("reserve") << true;
    }
    void lifecycleRequestsCaptureSubmissionGeneration() {
        QFETCH(QString, operation);
        QFETCH(bool, success);
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"choice\":0}");
        auto o = options(dir.path());
        auto writes = std::make_shared<std::atomic<int>>(0);
        auto pause = std::make_shared<std::atomic<bool>>(false);
        auto entered = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        o.fileFault = [writes, pause, entered, release, success](const QString &phase,
                                                                 const QString &) {
            if (phase != "write")
                return QString();
            ++*writes;
            if (pause->load()) {
                entered->release();
                release->acquire();
                return success ? QString() : QString("retry-write-fault");
            }
            return QString("initial-write-fault");
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto original = choice("head", 1);
        const auto capture = compat::request(store, [&] { return store.retainGroup(original); });
        const auto token = capture.groupToken;
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).status,
                 QString("failed"));
        const auto before = store.info({"choice"});
        const auto inputs = store.inputs().size();
        QMap<quint64, ConfigResult> results;
        connect(&store, &ConfigStore::completed, this,
                [&](const auto &result) { results[result.requestId] = result; });
        pause->store(true);
        const auto first = store.retryGroup(token);
        // Test scheduling only: keep this submitting turn until the actual write
        // is held, so no queued saving publication can refresh the public cache.
        const bool waiting = entered->tryAcquire(1, 10000);
        if (!waiting)
            release->release();
        QVERIFY(waiting);
        const auto cached = store.groupInfo(token).state;
        quint64 second = 0;
        if (operation == "retry")
            second = store.retryGroup(token);
        else if (operation == "release")
            second = store.releaseGroup(token);
        else if (operation == "retain")
            second = store.retainGroup(original);
        else
            second = store.retainGroup({"reservation", {{"choice"}}, {}, {}});
        // A regression must not leave a worker held after an assertion fails.
        pause->store(false);
        release->release();
        QTRY_VERIFY(results.contains(first) && results.contains(second));
        QCOMPARE(cached, QString("stopped"));
        QCOMPARE(results[first].status, success ? QString("saved") : QString("failed"));
        QCOMPARE(results[second].status, QString("failed"));
        QCOMPARE(results[second].error.code, QString("group-state"));
        QCOMPARE(writes->load(), 2);
        QCOMPARE(store.groupInfo(token).attempts.size(), 2);
        QCOMPARE(store.groupInfo(token).state, success ? QString("succeeded") : QString("stopped"));
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info({"choice"}).latestInput, before.latestInput);
        QCOMPARE(store.info({"choice"}).pathGeneration, before.pathGeneration);
        QCOMPARE(store.info({"choice"}).intentRevision, before.intentRevision);
        QCOMPARE(disk(file, "choice"), success ? 1 : 0);
        const auto lookup = compat::request(store, [&] { return store.retainGroup(original); });
        QCOMPARE(lookup.status, QString("retained"));
        QCOMPARE(lookup.groupToken, token);
        QCOMPARE(lookup.capturePrefix, capture.capturePrefix);
    }
    void doubleRetryInOneTurnCannotBecomeTwoAttempts() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto token = compat::request(store, [&] {
                               return store.retainGroup(choice("double", 1));
                           }).groupToken;
        control(store, [](auto &w) {
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::BeforeOperation});
        });
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).status,
                 QString("failed"));
        control(store, [](auto &w) {
            w.holdInputs(true);
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::BeforeOperation});
        });
        QMap<quint64, ConfigResult> results;
        connect(&store, &ConfigStore::completed, this,
                [&](const auto &result) { results[result.requestId] = result; });
        const auto first = store.retryGroup(token);
        const auto second = store.retryGroup(token);
        control(store, [](auto &w) { w.holdInputs(false); });
        QTRY_VERIFY(results.contains(first) && results.contains(second));
        QCOMPARE(results[first].status, QString("failed"));
        QCOMPARE(results[second].error.code, QString("group-state"));
        QCOMPARE(store.groupInfo(token).attempts.size(), 2);
        // The refusal does not consume a fresh explicit attempt or its body.
        const auto fresh = compat::request(store, [&] { return store.retryGroup(token); });
        QCOMPARE(fresh.status, QString("saved"));
        QCOMPARE(fresh.attemptNumber, quint64(3));
    }
    void staleDependencyRefusalDoesNotStrandTheNextExplicitRequest() {
        QTemporaryDir dir;
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto head = compat::request(store, [&] {
                              return store.retainGroup(choice("generation-head", 1));
                          }).groupToken;
        // Use a real file fault once, then repair permissions explicitly.
        QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto failed = compat::request(store, [&] { return store.saveGroup(head); });
        QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                      QFileDevice::ExeOwner));
        QCOMPARE(failed.status, QString("failed"));
        const auto suffix = compat::request(store, [&] {
                                return store.retainGroup(choice("generation-suffix", 2, {head}));
                            }).groupToken;
        control(store, [](auto &w) { w.holdInputs(true); });
        QMap<quint64, ConfigResult> results;
        connect(&store, &ConfigStore::completed, this,
                [&](const auto &result) { results[result.requestId] = result; });
        const auto retry = store.retryGroup(head);
        const auto stale = store.saveGroup(suffix);
        control(store, [](auto &w) { w.holdInputs(false); });
        QTRY_VERIFY(results.contains(retry) && results.contains(stale));
        QCOMPARE(results[retry].status, QString("saved"));
        QCOMPARE(results[stale].error.code, QString("group-state"));
        QCOMPARE(store.groupInfo(suffix).state, QString("retained"));
        QVERIFY(store.groupInfo(suffix).attempts.isEmpty());
        QCOMPARE(disk(dir.filePath("user.json"), "choice"), 1);
        const auto fresh = compat::request(store, [&] { return store.saveGroup(suffix); });
        QCOMPARE(fresh.status, QString("saved"));
        QCOMPARE(fresh.attemptNumber, quint64(1));
        QCOMPARE(disk(dir.filePath("user.json"), "choice"), 2);
        QCOMPARE(compat::request(store, [&] { return store.releaseGroup(suffix); }).status,
                 QString("released"));
    }
    void idempotentRequestsDoNotInventMutation_data() {
        QTest::addColumn<bool>("edits");
        QTest::addColumn<bool>("later");
        QTest::newRow("gate-probe") << true << false;
        QTest::newRow("duplicate-after-later-intent") << true << true;
        QTest::newRow("reservation-only") << false << false;
        QTest::newRow("reservation-after-later-intent") << false << true;
    }
    void idempotentRequestsDoNotInventMutation() {
        QFETCH(bool, edits);
        QFETCH(bool, later);
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto request = choice("same-key", 1);
        if (!edits) {
            QVERIFY(store.setUser({"choice"}, 1).accepted);
            request.edits.clear();
        }
        const auto original = compat::request(store, [&] { return store.retainGroup(request); });
        QCOMPARE(original.status, QString("retained"));
        if (later) {
            QVERIFY(store.setUser({"choice"}, 9).accepted);
            QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                     QString("reloaded"));
        }
        auto *draft = store.draft({{"choice"}});
        QVERIFY(draft->valid());
        QVERIFY(draft->setValue({"choice"}, 3).accepted);
        const auto before = store.info({"choice"});
        const auto prefix = store.snapshot().inputPrefix;
        const auto count = store.inputs().size();
        const auto duplicate = compat::request(store, [&] { return store.retainGroup(request); });
        QCOMPARE(duplicate.status, QString("retained"));
        QCOMPARE(duplicate.groupToken, original.groupToken);
        QCOMPARE(duplicate.capturePrefix, original.capturePrefix);
        QCOMPARE(duplicate.coverage.first().intentRevision,
                 original.coverage.first().intentRevision);
        QCOMPARE(store.snapshot().inputPrefix, prefix);
        QCOMPARE(store.inputs().size(), count);
        const auto after = store.info({"choice"});
        QCOMPARE(after.latestInput, before.latestInput);
        QCOMPARE(after.workerCovered, before.workerCovered);
        QCOMPARE(after.pathGeneration, before.pathGeneration);
        QCOMPARE(after.intentRevision, before.intentRevision);
        QCOMPARE(after.contentRevision, before.contentRevision);
        QCOMPARE(after.publicationGeneration, before.publicationGeneration);
        QVERIFY(draft->dirty());
        QVERIFY(!draft->conflicted());
        QCOMPARE(draft->value({"choice"}).toInt(), 3);
        QCOMPARE(store.value({"choice"}).toInt(), later ? 9 : 1);
    }
    void pendingDuplicateHasNoSecondIngress() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        control(store, [](auto &w) { w.holdInputs(true); });
        QMap<quint64, ConfigResult> results;
        connect(&store, &ConfigStore::completed, this,
                [&](const auto &result) { results[result.requestId] = result; });
        const auto first = store.retainGroup(choice("queued-same", 1));
        QVERIFY(store.setUser({"choice"}, 9).accepted);
        const auto prefix = store.info({"choice"}).latestInput;
        const auto duplicate = store.retainGroup(choice("queued-same", 1));
        QCOMPARE(store.info({"choice"}).latestInput, prefix);
        control(store, [](auto &w) { w.holdInputs(false); });
        QTRY_VERIFY(results.contains(duplicate));
        QCOMPARE(results[duplicate].groupToken, results[first].groupToken);
        QCOMPARE(results[duplicate].capturePrefix, results[first].capturePrefix);
        QCOMPARE(store.inputs().size(), 2);
        QCOMPARE(store.value({"choice"}).toInt(), 9);
    }
    void retryAndRefusedAttemptsDoNotCreateIntent() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto token = compat::request(store, [&] {
                               return store.retainGroup(choice("retry-fences", 1));
                           }).groupToken;
        auto *draft = store.draft({{"choice"}});
        QVERIFY(draft->setValue({"choice"}, 3).accepted);
        const auto before = store.info({"choice"});
        const auto count = store.inputs().size();
        const auto prefix = store.snapshot().inputPrefix;
        for (int attempt = 0; attempt < 2; ++attempt) {
            control(store, [](auto &w) {
                w.scheduleBackend({false, ConfigMemoryBackend::Fault::BeforeOperation});
            });
            QCOMPARE(compat::request(
                         store,
                         [&] { return attempt ? store.retryGroup(token) : store.saveGroup(token); })
                         .status,
                     QString("failed"));
            QCOMPARE(store.inputs().size(), count);
            QCOMPARE(store.snapshot().inputPrefix, prefix);
            const auto after = store.info({"choice"});
            QCOMPARE(after.pathGeneration, before.pathGeneration);
            QCOMPARE(after.intentRevision, before.intentRevision);
            QCOMPARE(after.contentRevision, before.contentRevision);
            QVERIFY(draft->dirty());
            QVERIFY(!draft->conflicted());
        }
        QCOMPARE(compat::request(store, [&] { return store.releaseGroup(token); }).error.code,
                 QString("group-state"));
        QVERIFY(!draft->conflicted());
    }
    void frozenRetryAndAutosaveIsolation() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"choice\":0,\"other\":0,\"opaque\":18446744073709551615}");
        auto o = options(dir.path());
        o.autoSave = true;
        o.autoSaveInterval = 5;
        auto faults = std::make_shared<std::atomic<int>>(1);
        o.fileFault = [faults](const QString &phase, const QString &) {
            if (phase == "write" && faults->exchange(0) == 1)
                return QString("group-fault");
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto retained =
            compat::request(store, [&] { return store.retainGroup(choice("head", 1)); });
        QCOMPARE(retained.status, QString("retained"));
        const auto token = retained.groupToken;
        QVERIFY(!token.isEmpty());
        QCOMPARE(store.value({"choice"}).toInt(), 1);
        const auto failed = compat::request(store, [&] { return store.saveGroup(token); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(store.groupInfo(token).state, QString("stopped"));
        QVERIFY(store.setUser({"choice"}, 9).accepted);
        QSignalSpy autosaves(&store, &ConfigStore::autoSaved);
        QVERIFY(store.setUser({"other"}, 7).accepted);
        QTRY_COMPARE(disk(file, "other"), 7);
        QCOMPARE(disk(file, "choice"), 0);
        QTRY_VERIFY(!autosaves.isEmpty());
        const auto autoResult = qvariant_cast<ConfigResult>(autosaves.last().first());
        QVERIFY(autoResult.excludedGroups.contains(token));
        for (const auto &coverage : autoResult.coverage)
            QVERIFY(coverage.path != ConfigPath{"choice"});
        const auto duplicate =
            compat::request(store, [&] { return store.retainGroup(choice("head", 1)); });
        QCOMPARE(duplicate.groupToken, token);
        QCOMPARE(duplicate.capturePrefix, retained.capturePrefix);
        QCOMPARE(
            compat::request(store, [&] { return store.retainGroup(choice("head", 2)); }).error.code,
            QString("group-key-mismatch"));
        QCOMPARE(compat::request(store, [&] { return store.flush(); }).error.code,
                 QString("unresolved-group"));
        QCOMPARE(compat::request(store, [&] { return store.releaseGroup(token); }).error.code,
                 QString("group-state"));
        bool signalSeen = false, callbackSeen = false;
        const auto connection =
            connect(&store, &ConfigStore::completed, this, [&](const ConfigResult &r) {
                if (r.operation == "retryGroup") {
                    signalSeen = true;
                    QCOMPARE(store.groupInfo(token).state, QString("succeeded"));
                    QCOMPARE(disk(file, "choice"), 1);
                }
            });
        const auto retry = compat::request(store, [&] {
            return store.retryGroup(token, this, [&](const ConfigResult &) {
                QVERIFY(signalSeen);
                callbackSeen = true;
                QVERIFY(store.setUser({"other"}, 11).accepted);
            });
        });
        disconnect(connection);
        QVERIFY(callbackSeen);
        QCOMPARE(retry.status, QString("saved"));
        QCOMPARE(retry.attemptNumber, quint64(2));
        QVERIFY(retry.attemptId != failed.attemptId);
        QCOMPARE(retry.capturePrefix, retained.capturePrefix);
        QCOMPARE(store.value({"choice"}).toInt(), 9);
        QCOMPARE(store.groupInfo(token).attempts.first().status, QString("failed"));
        QCOMPARE(compat::request(store, [&] { return store.releaseGroup(token); }).status,
                 QString("released"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).error.code,
                 QString("group-state"));
        const auto finalSave = compat::request(store, [&] { return store.save(); });
        QVERIFY(finalSave.status == "saved" || finalSave.status == "nothing_to_save");
        QVERIFY(finalSave.inputPrefix > retry.inputPrefix);
        QCOMPARE(disk(file, "choice"), 9);
        QCOMPARE(disk(file, "other"), 11);
        QVERIFY(compat::bytes(file).contains("18446744073709551615"));
    }
    void dependentSuffixNeverRecaptures() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        auto stop = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [stop](const QString &phase, const QString &) {
            return phase == "write" && stop->load() ? QString("stopped-head") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto a =
            compat::request(store, [&] { return store.retainGroup(choice("a", 1)); }).groupToken;
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(a); }).status,
                 QString("failed"));
        QCOMPARE(compat::request(
                     store, [&] { return store.retainGroup(choice("missing-causal-edge", 99)); })
                     .error.code,
                 QString("group-dependency"));
        const auto b = compat::request(store, [&] {
                           return store.retainGroup(choice("b", 2, {a}));
                       }).groupToken;
        const auto c = compat::request(store, [&] {
                           return store.retainGroup(choice("c", 3, {b}));
                       }).groupToken;
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(b); }).error.code,
                 QString("group-dependency-stopped"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(c); }).error.code,
                 QString("group-dependency-stopped"));
        QVERIFY(!QFile::exists(dir.filePath("user.json")));
        stop->store(false);
        QCOMPARE(compat::request(store, [&] { return store.retryGroup(a); }).status,
                 QString("saved"));
        QCOMPARE(disk(dir.filePath("user.json"), "choice"), 1);
        QCOMPARE(store.value({"choice"}).toInt(), 3);
        QCOMPARE(store.groupInfo(b).state, QString("stopped"));
        QCOMPARE(compat::request(store, [&] { return store.retryGroup(b); }).status,
                 QString("saved"));
        QCOMPARE(disk(dir.filePath("user.json"), "choice"), 2);
        QCOMPARE(compat::request(store, [&] { return store.retryGroup(c); }).status,
                 QString("saved"));
        QCOMPARE(disk(dir.filePath("user.json"), "choice"), 3);
        QCOMPARE(compat::request(store, [&] { return store.flush(); }).status,
                 QString("nothing_to_save"));
    }
    void partialRoutesRetainConfirmedCoverage() {
        QTemporaryDir dir;
        auto o = options();
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers.first().sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}};
        o.jsonFiles = {{"user", "a", dir.filePath("a.json")},
                       {"user", "b", dir.filePath("b.json")}};
        auto aWrites = std::make_shared<std::atomic<int>>(0),
             bWrites = std::make_shared<std::atomic<int>>(0);
        o.fileFault = [aWrites, bWrites](const QString &phase, const QString &file) {
            if (phase == "write") {
                if (file.endsWith("a.json"))
                    ++*aWrites;
                else if (++*bWrites == 1)
                    return QString("b-fault");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRetainedGroupOptions group{
            "pair", {{"a"}, {"b"}}, {{{"a", "n"}, true, 7}, {{"b", "n"}, true, 8}}, {}};
        const auto token =
            compat::request(store, [&] { return store.retainGroup(group); }).groupToken;
        const auto first = compat::request(store, [&] { return store.saveGroup(token); });
        QCOMPARE(first.publicationState, QString("partial"));
        QCOMPARE(first.coveredRevisions.size(), 1);
        const auto retry = compat::request(store, [&] { return store.retryGroup(token); });
        QCOMPARE(retry.status, QString("saved"));
        QCOMPARE(retry.coveredRevisions.size(), 2);
        QCOMPARE(aWrites->load(), 1);
        QCOMPARE(bWrites->load(), 2);
        QCOMPARE(disk(dir.filePath("a.json"), "n"), 7);
        QCOMPARE(disk(dir.filePath("b.json"), "n"), 8);
    }
    void destroyedConsumerAndBusyToken() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto token = compat::request(store, [&] {
                               return store.retainGroup(choice("memory", 4));
                           }).groupToken;
        control(store, [](ConfigWorker &w) {
            w.scheduleBackend({true, ConfigMemoryBackend::Fault::BeforeOperation});
        });
        auto *context = new QObject;
        bool callback = false;
        QMap<quint64, ConfigResult> observed;
        connect(&store, &ConfigStore::completed, this,
                [&](const ConfigResult &r) { observed[r.requestId] = r; });
        store.saveGroup(token, context, [&](const ConfigResult &) { callback = true; });
        QTRY_COMPARE(store.groupInfo(token).state, QString("saving"));
        const auto busy = store.retryGroup(token);
        delete context;
        control(store, [](ConfigWorker &w) { w.releaseBackend(); });
        QTRY_COMPARE(store.groupInfo(token).state, QString("stopped"));
        const auto retry = compat::request(store, [&] { return store.retryGroup(token); });
        QVERIFY(observed.contains(busy));
        QCOMPARE(observed.value(busy).error.code, QString("group-state"));
        QVERIFY(!callback);
        QCOMPARE(retry.status, QString("saved"));
        QCOMPARE(store.groupInfo(token).attempts.size(), 2);
        QCOMPARE(store.value({"choice"}).toInt(), 4);
    }
    void uncertainOwnWitnessAndForeignRefusal_data() {
        QTest::addColumn<bool>("foreign");
        QTest::newRow("own-output") << false;
        QTest::newRow("foreign-output") << true;
    }
    void uncertainOwnWitnessAndForeignRefusal() {
        QFETCH(bool, foreign);
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto token = compat::request(store, [&] {
                               return store.retainGroup(choice("uncertain", 5));
                           }).groupToken;
        control(store, [](ConfigWorker &w) {
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::AfterPublication});
        });
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).publicationState,
                 QString("uncertain"));
        if (foreign)
            control(store, [](ConfigWorker &w) {
                w.backend()->seed("user", encodeMemoryValue({{"choice", 99}}));
            });
        const auto retried = compat::request(store, [&] { return store.retryGroup(token); });
        QCOMPARE(retried.status, foreign ? QString("failed") : QString("saved"));
        if (foreign)
            QCOMPARE(retried.error.code, QString("source-preimage-conflict"));
    }
    void namespaceResetReservationAndUnknownRemoval() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"settings\":{\"a\":1,\"b\":2,\"opaque\":9}}");
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(D::group({{"settings", D::group({{"a", D::leaf(T::SafeInteger, 0)},
                                                          {"b", D::leaf(T::SafeInteger, 0)}})}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"settings"}, "user").accepted);
        ConfigRetainedGroupOptions group{"reset-a", {{"settings", "a"}}, {}, {}};
        const auto token =
            compat::request(store, [&] { return store.retainGroup(group); }).groupToken;
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("unresolved-group"));
        ConfigJsonCodec codec;
        auto raw = codec.decode(compat::bytes(file), "user").raw.value("settings").toMap();
        QVERIFY(raw.contains("a"));
        QVERIFY(!raw.contains("b"));
        QVERIFY(!raw.contains("opaque"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).status,
                 QString("saved"));
        QVERIFY(!codec.decode(compat::bytes(file), "user").raw.contains("settings"));
    }
    void fileUncertainReadbackNeverRebasesForeign_data() {
        QTest::addColumn<bool>("foreign");
        QTest::newRow("strict-own-file-witness") << false;
        QTest::newRow("strict-foreign-file-witness") << true;
    }
    void fileUncertainReadbackNeverRebasesForeign() {
        QFETCH(bool, foreign);
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"choice\":0,\"opaque\":18446744073709551615}");
        auto o = options(dir.path());
        auto fail = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [fail](const QString &phase, const QString &) {
            return phase == "readback" && fail->exchange(false) ? QString("readback-fault")
                                                                : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto token = compat::request(store, [&] {
                               return store.retainGroup(choice("file-uncertain", 5));
                           }).groupToken;
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).publicationState,
                 QString("uncertain"));
        QCOMPARE(disk(file, "choice"), 5);
        QVERIFY(store.setUser({"choice"}, 9).accepted);
        if (foreign)
            compat::put(file, "{\"choice\":99}");
        const auto retry = compat::request(store, [&] { return store.retryGroup(token); });
        QCOMPARE(retry.status, foreign ? QString("failed") : QString("saved"));
        if (foreign)
            QCOMPARE(retry.error.code, QString("source-preimage-conflict"));
        QCOMPARE(disk(file, "choice"), foreign ? 99 : 5);
        QCOMPARE(store.value({"choice"}).toInt(), 9);
        if (!foreign)
            QVERIFY(compat::bytes(file).contains("18446744073709551615"));
    }
    void equalResetAbaAndSemanticErrors() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto first = choice("equal", 0);
        const auto captured = compat::request(store, [&] { return store.retainGroup(first); });
        QVERIFY(captured.coverage.first().intentRevision > 0);
        QVERIFY(store.setUser({"choice"}, 1).accepted);
        QVERIFY(store.setUser({"choice"}, 0).accepted);
        const auto same = compat::request(store, [&] { return store.retainGroup(first); });
        QCOMPARE(same.capturePrefix, captured.capturePrefix);
        QCOMPARE(same.coverage.first().intentRevision, captured.coverage.first().intentRevision);
        auto bad = choice("bad", 1, {"missing-token"});
        QCOMPARE(compat::request(store, [&] { return store.retainGroup(bad); }).error.code,
                 QString("group-dependency"));
        bad = {"overlap", {{"choice"}}, {{{"choice"}, true, 1}, {{"choice"}, false, {}}}, {}};
        QCOMPARE(compat::request(store, [&] { return store.retainGroup(bad); }).error.code,
                 QString("group-selection"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup("unknown"); }).error.code,
                 QString("group-token"));
        QCOMPARE(compat::request(store, [&] { return store.retryGroup(captured.groupToken); })
                     .error.code,
                 QString("group-state"));
        QVERIFY(store.groupInfo("unknown").error.code == "group-token");
        QCOMPARE(
            compat::request(store, [&] { return store.saveGroup(captured.groupToken); }).status,
            QString("saved"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        ConfigRetainedGroupOptions reset{"reset", {{"choice"}}, {{{"choice"}, false, {}}}, {}};
        const auto token =
            compat::request(store, [&] { return store.retainGroup(reset); }).groupToken;
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).status,
                 QString("saved"));
        QCOMPARE(store.value({"choice"}).toInt(), 0);
    }
    void earlierQueuedSaveCannotPublishLaterCapture() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"choice"}, 1).accepted);
        control(store, [](ConfigWorker &w) {
            w.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
        });
        QMap<quint64, ConfigResult> results;
        connect(&store, &ConfigStore::completed, this,
                [&](const ConfigResult &r) { results[r.requestId] = r; });
        const auto earlier = store.save();
        const auto captureId = store.retainGroup(choice("queued", 2));
        QVERIFY(store.setUser({"choice"}, 3).accepted);
        QCOMPARE(store.value({"choice"}).toInt(), 3);
        control(store, [](ConfigWorker &w) { w.releaseBackend(); });
        QTRY_VERIFY(results.contains(captureId));
        QCOMPARE(results.value(earlier).inputPrefix, quint64(1));
        QCOMPARE(results.value(captureId).capturePrefix, quint64(2));
        int persisted = -1;
        control(store, [&](ConfigWorker &w) {
            const auto id =
                w.backend()->submit({ConfigBackendRequest::Operation::Read, "user", {}, {}});
            persisted = decodeMemoryValue(w.backend()->take(id)->bytes).value("choice").toInt();
        });
        QCOMPARE(persisted, 1);
        QCOMPARE(store.value({"choice"}).toInt(), 3);
        const auto token = results.value(captureId).groupToken;
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).status,
                 QString("saved"));
        control(store, [&](ConfigWorker &w) {
            const auto id =
                w.backend()->submit({ConfigBackendRequest::Operation::Read, "user", {}, {}});
            persisted = decodeMemoryValue(w.backend()->take(id)->bytes).value("choice").toInt();
        });
        QCOMPARE(persisted, 2);
        QCOMPARE(store.value({"choice"}).toInt(), 3);
    }
    void disjointOwnPublicationAdvancesUncertainWitnessAndRecoveryIsAsync() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto token = compat::request(store, [&] {
                               return store.retainGroup(choice("uncertain-async", 5));
                           }).groupToken;
        control(store, [](ConfigWorker &w) {
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::Readback});
        });
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(token); }).publicationState,
                 QString("uncertain"));
        QVERIFY(store.setUser({"other"}, 7).accepted);
        const auto partial = compat::request(store, [&] { return store.save(); });
        QCOMPARE(partial.error.code, QString("unresolved-group"));
        QCOMPARE(store.groupInfo(token).state, QString("stopped"));
        control(store, [](ConfigWorker &w) {
            w.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
        });
        bool completed = false, heartbeat = false;
        store.retryGroup(token, this, [&](const ConfigResult &result) {
            QCOMPARE(result.status, QString("saved"));
            completed = true;
        });
        QTRY_COMPARE(store.groupInfo(token).state, QString("saving"));
        QTimer::singleShot(0, this, [&] { heartbeat = true; });
        QTRY_VERIFY(heartbeat);
        QVERIFY(!completed);
        control(store, [](ConfigWorker &w) { w.releaseBackend(); });
        QTRY_VERIFY(completed);
        QCOMPARE(store.value({"choice"}).toInt(), 5);
        QCOMPARE(store.value({"other"}).toInt(), 7);
        QCOMPARE(compat::request(store, [&] { return store.flush(); }).status,
                 QString("nothing_to_save"));
    }
};
QTEST_GUILESS_MAIN(RetainedGroupsTest)
#include "test_retained_groups.moc"
