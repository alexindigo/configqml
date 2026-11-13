// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "confignumber.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSaveFile>
#include <QSignalSpy>
#include <atomic>
#include <algorithm>
#include <QSemaphore>
#include <QScopeGuard>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
ConfigStoreOptions options(const QString &directory) {
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"user", {}, {}}},
                 true}};
    o.jsonFiles = {{"user", "user", directory + "/user.json"}};
    o.observedPaths = {{"n"}};
    return o;
}
void replace(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        qFatal("source event replacement");
}
void control(ConfigStore &store, const std::function<void(ConfigWorker &)> &action) {
    bool done = false;
    ConfigStoreTestAccess::run(store, action, &store, [&] { done = true; });
    QTRY_VERIFY(done);
}
// The application-side deployment input consumes only accepted external disk
// choices. Preview invalidation, publication receipts and metadata never deploy.
struct DeploymentInput : QObject {
    QList<ConfigSourceAccepted> accepted, deployments;
    QList<QVariantList> previews;
    explicit DeploymentInput(ConfigStore &store) {
        QObject::connect(&store, &ConfigStore::sourceAccepted, this, [&](const auto &event) {
            accepted.append(event);
            QVariantList preview;
            for (const auto &fact : event.facts)
                preview.append(store.value(fact.path));
            previews.append(preview);
            if (event.origin == "external")
                deployments.append(event);
        });
    }
    QVariantList deployed() const {
        QVariantList values;
        for (const auto &fact : deployments.last().facts)
            values.append(fact.value);
        return values;
    }
};
} // namespace
class SourceEventsTest : public QObject {
    Q_OBJECT
private slots:
    void mixedUncertainPublicationOwners_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("groupFirst");
        for (int policy = 0; policy < 3; ++policy)
            for (bool groupFirst : {false, true})
                QTest::newRow(
                    qPrintable(QString("policy-%1-groupFirst-%2").arg(policy).arg(groupFirst)))
                    << policy << groupFirst;
    }
    void mixedUncertainPublicationOwners() {
        QFETCH(int, policy);
        QFETCH(bool, groupFirst);
        QTemporaryDir dir;
        compat::put(dir.filePath("user.json"), "{\"n\":0,\"other\":0}");
        auto o = options(dir.path());
        o.schema = ConfigSchema(
            D::group({{"n", D::leaf(T::SafeInteger, 0)}, {"other", D::leaf(T::SafeInteger, 0)}}));
        o.evidenceDirectory = dir.filePath("evidence");
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy(policy);
        auto faults = std::make_shared<std::atomic_int>(2);
        o.fileFault = [faults](const QString &phase, const QString &) {
            return phase == "readback" && faults->fetch_sub(phase == "readback" ? 1 : 0) > 0
                       ? QString("readback-fault")
                       : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QString token;
        if (groupFirst) {
            token = compat::request(store, [&] {
                        return store.retainGroup({"mixed", {{"n"}}, {{{"n"}, true, 1}}, {}});
                    }).groupToken;
        } else
            QVERIFY(store.setUser({"n"}, 1).accepted);
        const auto first = compat::request(
            store, [&] { return groupFirst ? store.saveGroup(token) : store.save(); });
        QCOMPARE(first.error.code, QString("readback-fault"));
        if (groupFirst)
            QVERIFY(store.setUser({"other"}, 2).accepted);
        else
            token = compat::request(store, [&] {
                        return store.retainGroup({"mixed", {{"n"}}, {{{"n"}, true, 2}}, {}});
                    }).groupToken;
        const auto second = compat::request(
            store, [&] { return groupFirst ? store.save() : store.saveGroup(token); });
        QCOMPARE(second.error.code, QString("readback-fault"));
        QCOMPARE(store.groupInfo(token).state, QString("stopped"));
        const auto retry = compat::request(store, [&] { return store.retryGroup(token); });
        QCOMPARE(retry.status, QString("saved"));
        QCOMPARE(store.groupInfo(token).state, QString("succeeded"));
        QCOMPARE(compat::request(store, [&] { return store.releaseGroup(token); }).status,
                 QString("released"));
        const auto saved = compat::request(store, [&] { return store.save(); });
        QVERIFY2(saved.status != "failed", qPrintable(saved.error.code));
        bool ordinary = true;
        control(store, [&](auto &worker) {
            ordinary = ConfigStoreTestAccess::ordinaryOwnPublication(worker, "user");
        });
        QVERIFY(!ordinary);
        auto clean = o;
        clean.fileFault = {};
        ConfigStore reopened(clean);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"n"}).toInt(), groupFirst ? 1 : 2);
        QCOMPARE(reopened.value({"other"}).toInt(), groupFirst ? 2 : 0);
    }
    void foreignSupersedesOrdinaryPathIntentRecovery_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("retained");
        QTest::addColumn<int>("discovery");
        QTest::addColumn<bool>("overlap");
        for (int policy = 0; policy < 3; ++policy)
            for (bool retained : {false, true})
                for (int discovery = 0; discovery < 3; ++discovery)
                    for (bool overlap : {false, true})
                        QTest::newRow(
                            qPrintable(QString("policy-%1-retained-%2-discovery-%3-overlap-%4")
                                           .arg(policy)
                                           .arg(retained)
                                           .arg(discovery)
                                           .arg(overlap)))
                            << policy << retained << discovery << overlap;
    }
    void foreignSupersedesOrdinaryPathIntentRecovery() {
        QFETCH(int, policy);
        QFETCH(bool, retained);
        QFETCH(int, discovery);
        QFETCH(bool, overlap);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"n\":0,\"other\":0,\"wide\":18446744073709551615}";
        const QByteArray foreign =
            overlap ? QByteArray("{\"n\":9,\"other\":7,\"wide\":18446744073709551615}")
                    : QByteArray("{\"n\":0,\"other\":7,\"wide\":18446744073709551615}");
        compat::put(path, original);
        auto o = options(dir.path());
        o.schema = ConfigSchema(
            D::group({{"n", D::leaf(T::SafeInteger, 0)}, {"other", D::leaf(T::SafeInteger, 0)}}));
        o.observedPaths = {{"n"}, {"other"}};
        o.evidenceDirectory = dir.filePath("evidence");
        o.jsonFiles[0].watchMode = discovery == 2 ? ConfigWatchMode::Live : ConfigWatchMode::Off;
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy(policy);
        auto once = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [once](const QString &phase, const QString &) {
            return phase == "readback" && once->exchange(false) ? QString("readback-fault")
                                                                : QString();
        };
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        if (discovery != 2)
            QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                     QString("reloaded"));
        consumer.deployments.clear();
        QString token;
        if (retained)
            token = compat::request(store, [&] {
                        return store.retainGroup({"review", {{"n"}}, {{{"n"}, true, 1}}, {}});
                    }).groupToken;
        else
            QVERIFY(store.setUser({"n"}, 1).accepted);
        const auto failed = compat::request(
            store, [&] { return retained ? store.saveGroup(token) : store.save(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.publicationState, QString("uncertain"));
        QCOMPARE(failed.error.code, QString("readback-fault"));
        replace(path, foreign);
        if (discovery == 1)
            QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                     QString("reloaded"));
        else if (discovery == 2)
            QTRY_COMPARE(consumer.deployments.size(), 1);
        if (discovery) {
            QCOMPARE(consumer.deployments.size(), 1);
            QCOMPARE(consumer.deployments.last().cause,
                     discovery == 2 ? QString("watch") : QString("reload"));
            QCOMPARE(store.value({"n"}).toInt(), 1); // Pending intent still masks the disk choice.
            QCOMPARE(store.value({"other"}).toInt(), 7);
        }
        const auto next = compat::request(
            store, [&] { return retained ? store.retryGroup(token) : store.save(); });
        const bool pathIntent = policy == 0 && !retained;
        if (!pathIntent) {
            QCOMPARE(next.status, QString("failed"));
            QCOMPARE(next.error.code, QString("source-preimage-conflict"));
            QCOMPARE(compat::bytes(path), foreign);
            return; // Existing strict and retained recovery contracts are unchanged.
        }
        QCOMPARE(next.status, overlap ? QString("failed") : QString("saved"));
        if (overlap) {
            QCOMPARE(next.error.code, QString("path-conflict"));
            QCOMPARE(compat::bytes(path), foreign);
        }
        bool witnessed = true;
        control(store, [&](auto &worker) {
            witnessed = ConfigStoreTestAccess::ordinaryOwnPublication(worker, "user");
        });
        QVERIFY(!witnessed);
        if (overlap) {
            replace(path, "{\"n\":0,\"other\":7,\"wide\":18446744073709551615}");
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        }
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(store.value({"n"}).toInt(), 1);
        QCOMPARE(store.value({"other"}).toInt(), 7);
        QVERIFY(compat::bytes(path).contains("18446744073709551615"));
        QVERIFY(!store.failures().isEmpty());
        QCOMPARE(store.failures().first().error.code, QString("readback-fault"));
        auto restartOptions = o;
        restartOptions.jsonFiles[0].watchMode = ConfigWatchMode::StartupSnapshot;
        ConfigStore restart(restartOptions);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"n"}).toInt(), 1);
        QCOMPARE(restart.value({"other"}).toInt(), 7);
    }
    void uncertainOwnEchoKeepsNewestChoice_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("retained");
        for (int policy = 0; policy < 3; ++policy)
            for (bool retained : {false, true})
                QTest::newRow(
                    qPrintable(QString("policy-%1-retained-%2").arg(policy).arg(retained)))
                    << policy << retained;
    }
    void uncertainOwnEchoKeepsNewestChoice() {
        QFETCH(int, policy);
        QFETCH(bool, retained);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"n\":0}\n");
        auto o = options(dir.path());
        o.jsonFiles[0].watchMode = ConfigWatchMode::Live;
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy(policy);
        o.evidenceDirectory = dir.filePath("evidence");
        auto entered = std::make_shared<QSemaphore>(), release = std::make_shared<QSemaphore>();
        auto once = std::make_shared<std::atomic_bool>(true);
        auto readbackReads = std::make_shared<std::atomic<quint64>>(0);
        auto backend = std::make_shared<ConfigJsonBackend *>(nullptr);
        o.fileFault = [=](const QString &phase, const QString &) {
            if (phase == "readback" && once->exchange(false)) {
                *readbackReads = (*backend)->reads("user") + 1;
                entered->release();
                if (!release->tryAcquire(1, 10000))
                    return QString("fixture-timeout");
                return QString("terminal-readback-fault");
            }
            return QString();
        };
        ConfigStore store(o);
        DeploymentInput consumer(store);
        int reRequests = 0;
        connect(&store, &ConfigStore::sourceAccepted, this, [&](const auto &event) {
            if (event.origin == "external") {
                ++reRequests; // Model the native application's external-choice forwarding.
                store.setUser({"n"}, event.facts.first().value);
            }
        });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        control(store, [&](auto &worker) { *backend = worker.jsonBackend(); });
        QString head, tail;
        if (retained) {
            head = compat::request(store, [&] {
                       return store.retainGroup({"head", {{"n"}}, {{{"n"}, true, 1}}, {}});
                   }).groupToken;
            tail = compat::request(store, [&] {
                       return store.retainGroup({"tail", {{"n"}}, {{{"n"}, true, 0}}, {head}});
                   }).groupToken;
            QVERIFY(!head.isEmpty() && !tail.isEmpty());
        } else
            QVERIFY(store.setUser({"n"}, 1).accepted);
        const auto unblock = qScopeGuard([release] { release->release(); });
        const auto first = retained ? store.saveGroup(head) : store.save();
        QVERIFY(entered->tryAcquire(1, 5000));
        if (!retained)
            QVERIFY(store.setUser({"n"}, 0).accepted); // The original newer tail, not a re-request.
        release->release();
        const auto failed = compat::request(store, [&] { return first; });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, QString("terminal-readback-fault"));
        QCOMPARE(failed.publicationState, QString("uncertain"));
        QVERIFY(!failed.coveredRevisions.contains("user"));
        quint64 reads = 0;
        QTRY_VERIFY(([&] {
            control(store, [&](auto &worker) { reads = worker.jsonBackend()->reads("user"); });
            return reads > readbackReads->load();
        })());
        QCOMPARE(reRequests, 0);
        QVERIFY(consumer.deployments.isEmpty());
        QCOMPARE(store.value({"n"}).toInt(), 0);
        if (retained) {
            QCOMPARE(store.groupInfo(head).state, QString("stopped"));
            QCOMPARE(store.groupInfo(head).attempts.size(), 1);
        }
        bool own = false;
        const auto ownEvidence = store.sourceStates().first().evidence;
        for (const auto &reference : ownEvidence)
            if (reference.role == "own" && reference.phase == "read") {
                const auto meta =
                    decodeJsonValue(compat::bytes(reference.directory + "/metadata.json"))
                        .value.toMap();
                QCOMPARE(meta.value("requestId").toString(), QString::number(failed.requestId));
                QCOMPARE(meta.value("groupToken").toString(), failed.groupToken);
                QCOMPARE(meta.value("attemptId").toString(), failed.attemptId);
                QCOMPARE(meta.value("revision").toString(),
                         QString::fromLatin1(failed.publications.first().revision.token));
                own = true;
            }
        QVERIFY(own);
        const auto retry = compat::request(
            store, [&] { return retained ? store.retryGroup(head) : store.save(); });
        QCOMPARE(retry.status, QString("saved"));
        bool retryOwn = false;
        for (const auto &reference : retry.evidence)
            if (reference.role == "own" && reference.phase == "readback") {
                const auto meta =
                    decodeJsonValue(compat::bytes(reference.directory + "/metadata.json"))
                        .value.toMap();
                QCOMPARE(meta.value("requestId").toString(), QString::number(failed.requestId));
                QCOMPARE(meta.value("groupToken").toString(), failed.groupToken);
                QCOMPARE(meta.value("attemptId").toString(), failed.attemptId);
                retryOwn = true;
            }
        QVERIFY(retryOwn);
        if (retained) {
            QCOMPARE(retry.groupToken, head);
            QCOMPARE(retry.requestKey, failed.requestKey);
            QCOMPARE(retry.capturePrefix, failed.capturePrefix);
            QCOMPARE(retry.attemptNumber, quint64(2));
            QVERIFY(retry.attemptId != failed.attemptId);
            QCOMPARE(compat::request(store, [&] { return store.saveGroup(tail); }).status,
                     QString("saved"));
        }
        QCOMPARE(reRequests, 0);
        QCOMPARE(store.value({"n"}).toInt(), 0);
        QCOMPARE(ConfigJsonCodec()
                     .decode(compat::bytes(path), "user")
                     .raw.value("n")
                     .value<ConfigNumber>()
                     .integer(false)
                     .value.toInt(),
                 0);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"n"}).toInt(), 0);
    }
    void uncertainOwnDoesNotHideForeign_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("retained");
        QTest::addColumn<bool>("sameBytes");
        for (int policy = 0; policy < 3; ++policy)
            for (bool retained : {false, true})
                for (bool sameBytes : {false, true})
                    QTest::newRow(qPrintable(QString("policy-%1-retained-%2-same-%3")
                                                 .arg(policy)
                                                 .arg(retained)
                                                 .arg(sameBytes)))
                        << policy << retained << sameBytes;
    }
    void uncertainOwnDoesNotHideForeign() {
        QFETCH(int, policy);
        QFETCH(bool, retained);
        QFETCH(bool, sameBytes);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"n\":0}");
        auto o = options(dir.path());
        o.evidenceDirectory = dir.filePath("evidence");
        o.jsonFiles[0].watchMode = ConfigWatchMode::Live;
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy(policy);
        auto once = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [once](const QString &phase, const QString &) {
            return phase == "readback" && once->exchange(false) ? QString("readback-fault")
                                                                : QString();
        };
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QString token;
        if (retained)
            token = compat::request(store, [&] {
                        return store.retainGroup({"head", {{"n"}}, {{{"n"}, true, 1}}, {}});
                    }).groupToken;
        else
            QVERIFY(store.setUser({"n"}, 1).accepted);
        const auto failed = compat::request(
            store, [&] { return retained ? store.saveGroup(token) : store.save(); });
        QCOMPARE(failed.publicationState, QString("uncertain"));
        QTRY_VERIFY(([&] {
            const auto refs = store.sourceStates().first().evidence;
            return std::any_of(refs.cbegin(), refs.cend(),
                               [](const auto &r) { return r.role == "own" && r.phase == "read"; });
        })());
        QVERIFY(consumer.deployments.isEmpty());
        const auto foreign = sameBytes ? compat::bytes(path) : QByteArray("{\"n\":7}");
        replace(path, foreign);
        QTRY_COMPARE(consumer.deployments.size(), 1);
        QCOMPARE(consumer.deployments.last().cause, QString("watch"));
        QCOMPARE(consumer.deployed().first().toInt(), sameBytes ? 1 : 7);
        QVERIFY(consumer.deployments.last().facts.first().revision !=
                failed.publications.first().revision);
        QCOMPARE(compat::bytes(path), foreign);
        if (retained) {
            QCOMPARE(store.groupInfo(token).state, QString("stopped"));
            QCOMPARE(store.groupInfo(token).attempts.size(), 1);
        }
    }
    void mixedOwnAndForeignRead_data() {
        QTest::addColumn<bool>("retained");
        QTest::newRow("ordinary") << false;
        QTest::newRow("retained") << true;
    }
    void mixedOwnAndForeignRead() {
        QFETCH(bool, retained);
        QTemporaryDir dir;
        const auto a = dir.filePath("a.json"), b = dir.filePath("b.json");
        compat::put(a, "{\"n\":0}");
        compat::put(b, "{\"n\":0}");
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}};
        o.jsonFiles = {{"user", "a", a}, {"user", "b", b}};
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        o.observedPaths = {{"a", "n"}, {"b", "n"}};
        o.evidenceDirectory = dir.filePath("evidence");
        auto once = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [once](const QString &phase, const QString &) {
            return phase == "readback" && once->exchange(false) ? QString("readback-fault")
                                                                : QString();
        };
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto originalA = consumer.accepted.last().facts.first().revision;
        QString token;
        if (retained)
            token =
                compat::request(store, [&] {
                    return store.retainGroup({"head", {{"a", "n"}}, {{{"a", "n"}, true, 1}}, {}});
                }).groupToken;
        else
            QVERIFY(store.setUser({"a", "n"}, 1).accepted);
        QCOMPARE(
            compat::request(store, [&] { return retained ? store.saveGroup(token) : store.save(); })
                .publicationState,
            QString("uncertain"));
        replace(b, "{\"n\":7}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(consumer.deployments.size(), 1);
        QCOMPARE(consumer.deployments.last().changedPaths, (QList<ConfigPath>{{"b", "n"}}));
        QCOMPARE(consumer.deployed(), QVariantList({qint64(0), qint64(7)}));
        QCOMPARE(consumer.deployments.last().facts.first().revision, originalA);
        QCOMPARE(store.value({"a", "n"}).toInt(), 1);
        QCOMPARE(store.value({"b", "n"}).toInt(), 7);
        QCOMPARE(compat::request(store,
                                 [&] { return retained ? store.retryGroup(token) : store.save(); })
                     .status,
                 QString("saved"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a", "n"}).toInt(), 1);
        QCOMPARE(restart.value({"b", "n"}).toInt(), 7);
    }
    void ownLiveEchoDoesNotDeploy() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.jsonFiles[0].watchMode = ConfigWatchMode::Live;
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(consumer.accepted.size(), 1);
        QCOMPARE(consumer.accepted.last().cause, QString("startup"));
        QVERIFY(store.setUser({"n"}, 9).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(consumer.accepted.size(), 2);
        QCOMPARE(consumer.accepted.last().origin, QString("own_save"));
        QCOMPARE(consumer.accepted.last().cause, QString("save"));
        QCOMPARE(consumer.previews.last().first().toInt(), 9);
        quint64 reads = 0;
        control(store, [&](auto &w) { reads = w.jsonBackend()->reads("user"); });
        // Exercise the actual directory watcher, including the queued own echo.
        compat::put(dir.filePath("watch-hint"), "hint");
        QTest::qWait(150);
        control(store, [&](auto &w) { QCOMPARE(w.jsonBackend()->reads("user"), reads); });
        QCOMPARE(consumer.accepted.size(), 2);
        QVERIFY(consumer.deployments.isEmpty());
        // A foreign inode with identical values is a new accepted identity.
        const auto oldRevision = consumer.accepted.last().facts.first().revision;
        replace(dir.filePath("user.json"), "{\"n\":9}");
        QTRY_COMPARE(consumer.deployments.size(), 1);
        QCOMPARE(consumer.deployments.last().cause, QString("watch"));
        QVERIFY(consumer.deployments.last().facts.first().revision != oldRevision);
        QCOMPARE(consumer.deployed().first().toInt(), 9);
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(consumer.deployments.size(), 1);
    }
    void coherentCrossSectionAcceptance() {
        QTemporaryDir dir;
        compat::put(dir.filePath("a.json"), "{\"n\":1}");
        compat::put(dir.filePath("b.json"), "{\"n\":2}");
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}};
        o.jsonFiles = {{"user", "a", dir.filePath("a.json")},
                       {"user", "b", dir.filePath("b.json")}};
        o.observedPaths = {{"a", "n"}, {"b", "n"}};
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        replace(dir.filePath("a.json"), "{\"n\":3}");
        replace(dir.filePath("b.json"), "{\"n\":4}");
        bool callback = false;
        QCOMPARE(compat::request(store,
                                 [&] {
                                     return store.reload(this, [&](const auto &) {
                                         QCOMPARE(consumer.deployments.size(), 1);
                                         QCOMPARE(consumer.deployed(),
                                                  QVariantList({qint64(3), qint64(4)}));
                                         callback = true;
                                     });
                                 })
                     .status,
                 QString("reloaded"));
        QVERIFY(callback);
        QCOMPARE(consumer.accepted.size(), 2);
        QCOMPARE(consumer.previews.last(), consumer.deployed());
        QCOMPARE(consumer.deployments.last().cause, QString("reload"));
        QCOMPARE(consumer.deployments.last().sources.size(), 2);
        QCOMPARE(consumer.deployments.last().changedPaths, o.observedPaths);
    }
    void staleInspectorEvidenceIsNotAcceptance() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"n\":1}");
        auto o = options(dir.path());
        o.evidenceDirectory = dir.filePath("evidence");
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        control(store, [](auto &w) { w.holdReplies(true); });
        replace(file, "stale malformed observation");
        const auto failedId = store.reload();
        control(store, [](auto &) {});
        replace(file, "{\"n\":5}");
        const auto repairedId = store.reload();
        control(store, [](auto &) {});
        QVERIFY(store.setUser({"n"}, 9).accepted);
        QMap<quint64, ConfigResult> results;
        connect(&store, &ConfigStore::completed, this,
                [&](const auto &result) { results[result.requestId] = result; });
        control(store, [](auto &w) { w.releaseReplies(1); });
        QTRY_VERIFY(results.contains(failedId));
        QCOMPARE(results[failedId].status, QString("failed"));
        const auto evidence = store.sourceStates().first().evidence;
        QVERIFY(!evidence.isEmpty());
        QCOMPARE(compat::bytes(evidence.last().directory + "/bytes"),
                 QByteArray("stale malformed observation"));
        QCOMPARE(consumer.accepted.size(), 1);
        QVERIFY(consumer.deployments.isEmpty());
        QCOMPARE(store.value({"n"}).toInt(), 9);
        control(store, [](auto &w) { w.holdReplies(false); });
        QTRY_VERIFY(results.contains(repairedId));
        QCOMPARE(consumer.deployments.size(), 1);
        QCOMPARE(consumer.deployed().first().toInt(), 5);
        QCOMPARE(consumer.previews.last().first().toInt(), 9);
    }
    void failedSaveInvalidatesPreviewWithoutDeploying() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"n\":1}");
        auto o = options(dir.path());
        o.fileFault = [](const QString &phase, const QString &) {
            return phase == "write" ? QString("save-fault") : QString();
        };
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        int invalidations = 0;
        connect(&store, &ConfigStore::valuesChanged, this, [&](const auto &) {
            ++invalidations;
            QCOMPARE(store.value({"n"}).toInt(), 9);
        });
        QVERIFY(store.setUser({"n"}, 9).accepted);
        QCOMPARE(invalidations, 1);
        QVERIFY(consumer.deployments.isEmpty());
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("failed"));
        QCOMPARE(compat::bytes(file), QByteArray("{\"n\":1}"));
        QCOMPARE(consumer.accepted.size(), 1);
        replace(file, "{\"n\":5}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(consumer.deployments.size(), 1);
        QCOMPARE(consumer.deployed().first().toInt(), 5);
        QCOMPARE(store.value({"n"}).toInt(), 9);
    }
    void fallbackDiskFactsIgnorePendingWholeObjectSelection() {
        QTemporaryDir dir;
        const auto user = dir.filePath("user.json");
        const QByteArray legacy("{\"a\":1,\"b\":2}");
        compat::put(dir.filePath("legacy.json"), legacy);
        compat::put(user, "{}");
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(D::group({{"settings", D::group({{"a", D::leaf(T::SafeInteger, 0)},
                                                          {"b", D::leaf(T::SafeInteger, 0)}})}}));
        ConfigSource fallback{"legacy", {"settings"}, {}};
        fallback.fallbackFor = "user";
        o.layers = {
            {"legacy", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {fallback}},
            {"user",
             ConfigLayerKind::File,
             ConfigAdmissionPolicy::FieldFallback,
             {{"user", {"settings"}, {}}},
             true}};
        o.jsonFiles = {{"legacy", "legacy", dir.filePath("legacy.json")}, {"user", "user", user}};
        o.observedPaths = {{"settings", "a"}, {"settings", "b"}};
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(consumer.accepted.last().facts[1].value.toInt(), 2);
        QVERIFY(store.setUser({"settings", "a"}, 9).accepted);
        QTRY_COMPARE(store.info({"settings", "a"}).workerCovered, quint64(1));
        QCOMPARE(store.value({"settings", "b"}).toInt(), 0);
        QCOMPARE(consumer.accepted.size(), 1);
        replace(user, "{\"a\":3}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(consumer.deployments.size(), 1);
        QCOMPARE(consumer.deployed(), QVariantList({qint64(3), qint64(0)}));
        QCOMPARE(consumer.deployments.last().facts[0].sourceId, QString("user"));
        QCOMPARE(consumer.deployments.last().facts[1].sourceId, QString("defaults"));
        QCOMPARE(consumer.previews.last(), QVariantList({qint64(9), qint64(0)}));
        replace(user, "{}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(consumer.deployments.size(), 2);
        QCOMPARE(consumer.deployed(), QVariantList({qint64(1), qint64(2)}));
        QCOMPARE(consumer.deployments.last().facts[1].sourceId, QString("legacy"));
        QCOMPARE(consumer.previews.last(), QVariantList({qint64(9), qint64(0)}));
        QCOMPARE(compat::bytes(dir.filePath("legacy.json")), legacy);
        QCOMPARE(compat::bytes(user), QByteArray("{}"));
    }
    void metadataProjectionDoesNotDeploy() {
        QTemporaryDir dir;
        compat::put(dir.filePath("user.json"),
                    "{\"rows\":[{\"id\":\"keep\"},{\"id\":\"remove\"}]}");
        auto o = options(dir.path());
        auto row = D::leaf(T::Record, QVariantMap{{"id", QString()}});
        row.fields = {{"id", D::leaf(T::String, QString())}};
        o.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        o.observedPaths = {{"rows"}};
        ConfigRecordListPolicySpec policy;
        policy.policyId = "rows";
        policy.path = {"rows"};
        policy.keyField = "id";
        policy.pruneUnknown = true;
        o.recordListPolicies = {policy};
        ConfigStore store(o);
        DeploymentInput consumer(store);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRecordListMetadata metadata;
        metadata.ready = true;
        metadata.entries = {{"keep", true, false, false, {}}};
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("rows", metadata); })
                .status,
            QString("projected"));
        QCOMPARE(consumer.accepted.last().origin, QString("projection"));
        QCOMPARE(consumer.accepted.last().cause, QString("metadata"));
        QCOMPARE(consumer.accepted.last().facts.first().value.toList().size(), 2);
        QCOMPARE(store.value({"rows"}).toList().size(), 1);
        QVERIFY(consumer.deployments.isEmpty());
        QCOMPARE(store.inputs().size(), 0);
    }
    void diskFactsRemainSeparateFromLocalDisplay() {
        QTemporaryDir dir;
        compat::put(dir.filePath("user.json"), "{\"n\":1}");
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        o.jsonFiles = {{"user", "user", dir.filePath("user.json")}};
        o.observedPaths = {{"n"}};
        ConfigStore store(o);
        QList<ConfigSourceAccepted> events;
        connect(&store, &ConfigStore::sourceAccepted, this,
                [&](const auto &event) { events.append(event); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.last().origin, QString("startup"));
        QVERIFY(store.setUser({"n"}, 7).accepted);
        compat::put(dir.filePath("user.json"), "{\"n\":5}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(store.value({"n"}).toInt(), 7);
        QCOMPARE(events.last().origin, QString("external"));
        QCOMPARE(events.last().facts.first().value.toInt(), 5);
        const auto count = events.size();
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(events.size(), count);
    }
};
QTEST_GUILESS_MAIN(SourceEventsTest)
#include "test_source_events.moc"
