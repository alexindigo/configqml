// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSaveFile>
#include <QSignalSpy>
#include <atomic>
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
