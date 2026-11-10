// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "fileoptions.h"
#include "confignumber.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSaveFile>
#include <QDir>
#include <atomic>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
void replace(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        qFatal("evidence backlog replacement fixture");
}
QVariantMap metadata(const ConfigEvidenceReference &reference) {
    return decodeJsonValue(compat::bytes(reference.directory + "/metadata.json")).value.toMap();
}
ConfigStoreOptions options(const QString &directory) {
    auto o = fileOptions(directory + "/user.json");
    o.schema = ConfigSchema(D::group({{"scale", D::leaf(T::Real, 1.0)}}));
    o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
    o.evidenceDirectory = directory + "/evidence";
    return o;
}
void control(ConfigStore &store, std::function<void(ConfigWorker &)> action) {
    bool done = false;
    ConfigStoreTestAccess::run(store, std::move(action), &store, [&] { done = true; });
    QTRY_VERIFY(done);
}
} // namespace
class EvidenceBacklogTest : public QObject {
    Q_OBJECT
private slots:
    void acquiredForeignSurvivesRestoredPreimageAndRetentionFailure() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"scale\":1.0}\n";
        const QByteArray foreign = "{\"scale\":1.2,\"futureRaw\":{\"helper\":\"preimage\","
                                   "\"values\":[false,0,null,\"\"]}}\n";
        compat::put(path, original);
        auto o = options(dir.path());
        auto once = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [=](const QString &phase, const QString &) {
            if (phase == "pending-accept" && once->exchange(false)) {
                if (!QFile::remove(path) || !QFile::rename(path + ".p", path) ||
                    !QDir().rename(o.evidenceDirectory, o.evidenceDirectory + ".held"))
                    qFatal("restore P and obstruct evidence fixture");
                compat::put(o.evidenceDirectory, "obstructed");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"scale"}, 1.7).accepted);
        QVERIFY(QFile::rename(path, path + ".p")); // Restore the exact original inode, not a copy.
        replace(path, foreign);
        ConfigJsonBackend observer(o.jsonFiles);
        const auto read = *observer.take(
            observer.submit({ConfigBackendRequest::Operation::Read, "json", {}, {}}));
        QVERIFY(read.success);
        QCOMPARE(read.bytes, foreign);
        const auto failed = compat::request(store, [&] { return store.save(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, QString("evidence-retention"));
        QCOMPARE(compat::bytes(path), original);
        QVERIFY(QFile::remove(o.evidenceDirectory));
        QVERIFY(QDir().rename(o.evidenceDirectory + ".held", o.evidenceDirectory));
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        const auto raw = ConfigJsonCodec().decode(compat::bytes(path), "json").raw;
        QCOMPARE(raw.value("scale").value<ConfigNumber>().real().value.toDouble(), 1.7);
        QVERIFY(!raw.contains("futureRaw")); // F is evidence, not the restored current choice.
        bool found = false;
        for (const auto &reference : saved.evidence) {
            if (reference.role != "foreign" ||
                compat::bytes(reference.directory + "/bytes") != foreign)
                continue;
            const auto meta = metadata(reference);
            QCOMPARE(meta.value("sourceId").toString(), QString("json"));
            QCOMPARE(meta.value("phase").toString(), QString("preflight"));
            QCOMPARE(meta.value("requestId").toString(), QString::number(failed.requestId));
            QCOMPARE(meta.value("revision").toString(), QString::fromLatin1(read.revision.token));
            QCOMPARE(meta.value("observationOrder").toString(),
                     QString::number(reference.observationOrder));
            QVERIFY(!found); // Failed pending-accept and transaction delivery share one witness.
            found = true;
        }
        QVERIFY2(found, "exact acquired foreign evidence must survive failed retention and retry");
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"scale"}).toDouble(), 1.7);
    }
    void boundedOverflowPreservesEveryWitnessAndScopesBackpressure() {
        QTemporaryDir dir;
        const auto a = dir.filePath("a.json"), b = dir.filePath("b.json");
        const QByteArray original = "{\"n\":0}\n";
        compat::put(a, original);
        compat::put(b, original);
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}};
        o.jsonFiles = {{"user", "a", a}, {"user", "b", b}};
        for (auto &file : o.jsonFiles)
            file.conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        auto blocked = std::make_shared<std::atomic_bool>(false);
        o.fileFault = [=](const QString &phase, const QString &file) {
            return phase == "evidence-retain" && file == a && *blocked ? QString("blocked")
                                                                       : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        *blocked = true;
        QList<QByteArray> bodies;
        QList<quint64> requests;
        const auto limit = ConfigStoreTestAccess::evidenceBacklogLimit();
        for (qsizetype i = 0; i < limit; ++i) {
            bodies.append("complete invalid acquired body " + QByteArray::number(i));
            replace(a, bodies.last());
            const auto result = compat::request(store, [&] { return store.reload(); });
            QCOMPARE(result.status, QString("failed"));
            QCOMPARE(result.error.code,
                     i + 1 == limit ? QString("evidence-backlog") : QString("evidence-retention"));
            requests.append(result.requestId);
        }
        QList<ConfigEvidenceObservation> pending;
        quint64 reads = 0;
        control(store, [&](auto &worker) {
            pending = ConfigStoreTestAccess::pendingEvidence(worker, "a");
            reads = worker.jsonBackend()->reads("a");
        });
        QCOMPARE(pending.size(), limit);
        for (qsizetype i = 0; i < limit; ++i) {
            QCOMPARE(pending[i].bytes, bodies[i]);
            QCOMPARE(pending[i].requestId, QString::number(requests[i]));
            if (i)
                QVERIFY(pending[i - 1].observationOrder < pending[i].observationOrder);
        }
        replace(a, "not acquired while saturated");
        replace(b, "{\"n\":5}\n");
        const auto reload = compat::request(store, [&] { return store.reload(); });
        QCOMPARE(reload.status, QString("failed"));
        QCOMPARE(reload.error.code, QString("evidence-backlog"));
        QCOMPARE(reload.error.sourceId, QString("a"));
        QCOMPARE(store.value({"a", "n"}).toInt(), 0);
        QCOMPARE(store.value({"b", "n"}).toInt(), 5); // Other-source admission still proceeds.
        QVERIFY(store.setUser({"a", "n"}, 17).accepted);
        QVERIFY(store.setUser({"b", "n"}, 9).accepted);
        const auto partial = compat::request(store, [&] { return store.save(); });
        QCOMPARE(partial.status, QString("failed"));
        QCOMPARE(partial.error.code, QString("evidence-backlog"));
        QCOMPARE(partial.publicationState, QString("partial"));
        QVERIFY(partial.coveredRevisions.contains("b"));
        QCOMPARE(ConfigJsonCodec()
                     .decode(compat::bytes(b), "b")
                     .raw.value("n")
                     .value<ConfigNumber>()
                     .integer(false)
                     .value.toInt(),
                 9);
        QCOMPARE(compat::bytes(a), QByteArray("not acquired while saturated"));
        control(store, [&](auto &worker) {
            QCOMPARE(worker.jsonBackend()->reads("a"),
                     reads); // No preflight or reload acquisition.
            const auto kept = ConfigStoreTestAccess::pendingEvidence(worker, "a");
            QCOMPARE(kept.size(), limit);
            for (qsizetype i = 0; i < limit; ++i) {
                QCOMPARE(kept[i].bytes, pending[i].bytes);
                QCOMPARE(kept[i].revision, pending[i].revision);
                QCOMPARE(kept[i].observationOrder, pending[i].observationOrder);
            }
        });
        replace(a, original);
        *blocked = false;
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(ConfigJsonCodec()
                     .decode(compat::bytes(a), "a")
                     .raw.value("n")
                     .value<ConfigNumber>()
                     .integer(false)
                     .value.toInt(),
                 17);
        QCOMPARE(store.value({"a", "n"}).toInt(), 17);
        QVERIFY(saved.evidence.size() >= limit);
        for (qsizetype i = 0; i < limit; ++i) {
            const auto &reference = saved.evidence[i];
            QCOMPARE(reference.sourceId, QString("a"));
            QCOMPARE(reference.observationOrder, pending[i].observationOrder);
            QCOMPARE(compat::bytes(reference.directory + "/bytes"), bodies[i]);
            const auto meta = metadata(reference);
            QCOMPARE(meta.value("requestId").toString(), QString::number(requests[i]));
            QCOMPARE(meta.value("revision").toString(),
                     QString::fromLatin1(pending[i].revision.token));
            QCOMPARE(meta.value("phase").toString(), QString("read"));
            QCOMPARE(meta.value("role").toString(), QString("foreign"));
        }
        control(store, [&](auto &worker) {
            QVERIFY(ConfigStoreTestAccess::pendingEvidence(worker, "a").isEmpty());
        });
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QVERIFY(store.setUser({"a", "n"}, 18).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
    }
    void publishedAndReadbackEvidenceSurvivesFailure_data() {
        QTest::addColumn<bool>("late");
        QTest::addColumn<bool>("retained");
        QTest::newRow("confirmed-readback") << false << false;
        QTest::newRow("foreign-late-readback") << true << false;
        QTest::newRow("retained-confirmed-readback") << false << true;
    }
    void publishedAndReadbackEvidenceSurvivesFailure() {
        QFETCH(bool, late);
        QFETCH(bool, retained);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"scale\":1.0}\n";
        const QByteArray foreign = "{\"scale\":1.2,\"opaque\":false}\n";
        compat::put(path, original);
        auto o = options(dir.path());
        auto once = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [=](const QString &phase, const QString &) {
            if (phase == "readback" && once->exchange(false)) {
                if (late)
                    replace(path, foreign);
                if (!QDir().rename(o.evidenceDirectory, o.evidenceDirectory + ".held"))
                    qFatal("readback evidence obstruction");
                compat::put(o.evidenceDirectory, "obstructed");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QString token;
        if (retained) {
            const ConfigRetainedGroupOptions group{
                "backlog", {{"scale"}}, {{{"scale"}, true, 1.7}}, {}};
            const auto captured = compat::request(store, [&] { return store.retainGroup(group); });
            QCOMPARE(captured.status, QString("retained"));
            token = captured.groupToken;
        } else
            QVERIFY(store.setUser({"scale"}, 1.7).accepted);
        const auto failed = compat::request(
            store, [&] { return retained ? store.saveGroup(token) : store.save(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, QString("evidence-retention"));
        QList<ConfigEvidenceObservation> pending;
        control(store, [&](auto &worker) {
            pending = ConfigStoreTestAccess::pendingEvidence(worker, "json");
        });
        QVERIFY(!pending.isEmpty());
        bool capturedPublished = false, capturedReadback = false;
        for (const auto &observation : pending) {
            capturedPublished |= observation.role == "published";
            capturedReadback |= observation.role == (late ? "late" : "readback");
        }
        QVERIFY(capturedPublished);
        QVERIFY(capturedReadback);
        QVERIFY(QFile::remove(o.evidenceDirectory));
        QVERIFY(QDir().rename(o.evidenceDirectory + ".held", o.evidenceDirectory));
        if (late)
            replace(path,
                    original); // Frozen strict work resolves against P, not the late stranger.
        const auto saved = compat::request(
            store, [&] { return retained ? store.retryGroup(token) : store.save(); });
        QCOMPARE(saved.status, late ? QString("saved") : QString("nothing_to_save"));
        QCOMPARE(ConfigJsonCodec()
                     .decode(compat::bytes(path), "json")
                     .raw.value("scale")
                     .value<ConfigNumber>()
                     .real()
                     .value.toDouble(),
                 1.7);
        for (const auto &observation : pending) {
            bool found = false;
            for (const auto &reference : saved.evidence)
                if (reference.observationOrder == observation.observationOrder) {
                    QCOMPARE(compat::bytes(reference.directory + "/bytes"), observation.bytes);
                    QCOMPARE(metadata(reference).value("requestId").toString(),
                             QString::number(failed.requestId));
                    QCOMPARE(metadata(reference).value("groupToken").toString(), failed.groupToken);
                    QCOMPARE(metadata(reference).value("attemptId").toString(), failed.attemptId);
                    QCOMPARE(metadata(reference).value("revision").toString(),
                             QString::fromLatin1(observation.revision.token));
                    found = true;
                }
            QVERIFY(found);
        }
    }
};
QTEST_GUILESS_MAIN(EvidenceBacklogTest)
#include "test_evidence_backlog.moc"
