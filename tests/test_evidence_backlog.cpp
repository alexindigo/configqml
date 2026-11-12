// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "fileoptions.h"
#include "confignumber.h"
#include "storeprobe.h"
#include "configdraft.h"
#include "configstorehandle.h"
#include <QTemporaryDir>
#include <QSaveFile>
#include <QDir>
#include <atomic>
#include <algorithm>

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
    void applyRetryAfterEvidenceRefusal_data() {
        QTest::addColumn<bool>("overlay");
        QTest::addColumn<bool>("backlog");
        QTest::addColumn<int>("policy");
        for (bool overlay : {false, true})
            for (bool backlog : {false, true})
                for (int policy = 0; policy < 3; ++policy)
                    QTest::newRow(qPrintable(QString("%1-%2-policy-%3")
                                                 .arg(overlay ? "overlay" : "draft")
                                                 .arg(backlog ? "backlog" : "retention")
                                                 .arg(policy)))
                        << overlay << backlog << policy;
    }
    void applyRetryAfterEvidenceRefusal() {
        QFETCH(bool, overlay);
        QFETCH(bool, backlog);
        QFETCH(int, policy);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original =
            overlay ? QByteArray("{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"scale\":1.0,"
                                 "\"opaque\":true}}],\"wide\":18446744073709551615}")
                    : QByteArray("{\"scale\":1.0,\"wide\":18446744073709551615}");
        compat::put(path, original);
        auto o = options(dir.path());
        if (overlay) {
            auto row = D::leaf(T::Record, QVariantMap{{"id", QString()}});
            row.fields = {{"id", D::leaf(T::String, QString())}};
            row.open = true;
            o.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        }
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy(policy);
        auto armed = std::make_shared<std::atomic_bool>(false);
        auto blocked = std::make_shared<std::atomic_bool>(false);
        auto retains = std::make_shared<std::atomic_int>(0);
        auto admissions = std::make_shared<std::atomic_int>(0);
        auto worker = std::make_shared<ConfigWorker *>(nullptr);
        auto saturation = std::make_shared<QString>();
        o.fileFault = [=](const QString &phase, const QString &) {
            if (phase != "evidence-retain")
                return QString();
            if (*armed && !backlog && retains->fetch_add(1) >= 1)
                *blocked = true; // Refresh succeeds; publication's original evidence fails.
            return *blocked ? QString("blocked") : QString();
        };
        o.admission = [=](const ConfigInput &) {
            ++*admissions;
            if (*armed && backlog) {
                *blocked = true;
                // Real worker reads fill the FIFO after selected refresh and
                // before admission/publication. They do not change accepted data.
                for (qsizetype i = 0; i < ConfigStoreTestAccess::evidenceBacklogLimit(); ++i)
                    *saturation = ConfigStoreTestAccess::acquireEvidence(**worker, "json");
            }
            return QString();
        };
        ConfigStoreOptions baseOptions;
        baseOptions.autoSave = false;
        baseOptions.schema =
            ConfigSchema(D::group({{"settings", D::group({{"scale", D::leaf(T::Real, 1.0)}})}}));
        ConfigStore base(baseOptions), store(o);
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        control(store, [&](auto &w) { *worker = &w; });
        ConfigDraft *draft = nullptr;
        ConfigOverlayEditor *editor = nullptr;
        if (overlay) {
            connect(&store, &ConfigStore::overlayOpened, this,
                    [&](quint64, ConfigOverlayEditor *created) { editor = created; });
            ConfigOverlayOptions selection;
            selection.base = base.scope({"settings"}, false);
            selection.ownerPath = {"rows"};
            selection.ownerGeneration = store.info({"rows"}).publicationGeneration;
            selection.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
            selection.identityPath = {{ConfigValueSelector::Kind::Key, "id"}};
            selection.identityValue = QString("x");
            selection.overrideKey = "instanceSettings";
            QCOMPARE(compat::request(store, [&] { return store.openOverlay(selection); }).status,
                     QString("opened"));
            QVERIFY(editor && editor->setValue({"scale"}, 1.7).accepted);
        } else {
            draft = store.draft({{"scale"}});
            QVERIFY(draft->valid());
            QVERIFY(draft->setValue({"scale"}, 1.7).accepted);
        }
        *armed = true;
        const auto first =
            compat::request(store, [&] { return overlay ? editor->apply() : draft->apply(); });
        QCOMPARE(first.status, QString("failed"));
        QCOMPARE(first.error.code,
                 backlog ? QString("evidence-backlog") : QString("evidence-retention"));
        QCOMPARE(compat::bytes(path), original);
        QVERIFY(first.publicationState != "uncertain");
        for (const auto &fact : first.publications)
            QVERIFY(!fact.success);
        QCOMPARE(admissions->load(), 1);
        QVERIFY(overlay ? editor->dirty() : draft->dirty());
        QVERIFY(!(overlay ? editor->conflicted() : draft->conflicted()));
        const ConfigPath leaf = overlay ? ConfigPath{"rows"} : ConfigPath{"scale"};
        const auto accepted = store.info(leaf);
        QVERIFY(accepted.intentRevision > 0);
        const auto inputs = store.inputs().size();
        QList<ConfigEvidenceObservation> pending;
        control(store,
                [&](auto &w) { pending = ConfigStoreTestAccess::pendingEvidence(w, "json"); });
        QVERIFY(!pending.isEmpty());
        if (backlog) {
            QCOMPARE(*saturation, QString("evidence-backlog"));
            QCOMPARE(pending.size(), ConfigStoreTestAccess::evidenceBacklogLimit());
        } else
            QCOMPARE(pending.first().requestId, QString::number(first.requestId));
        *armed = *blocked = false;
        const auto retry =
            compat::request(store, [&] { return overlay ? editor->apply() : draft->apply(); });
        QCOMPARE(retry.status, QString("saved"));
        if (overlay)
            QCOMPARE(retry.refreshState, QString("current"));
        QCOMPARE(admissions->load(), 1);
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info(leaf).intentRevision, accepted.intentRevision);
        QCOMPARE(store.info(leaf).pathGeneration, accepted.pathGeneration);
        QCOMPARE(store.info(leaf).latestInput, accepted.latestInput);
        QVERIFY(!(overlay ? editor->dirty() : draft->dirty()));
        QVERIFY(!(overlay ? editor->conflicted() : draft->conflicted()));
        for (const auto &observation : pending) {
            const auto found = std::find_if(
                retry.evidence.cbegin(), retry.evidence.cend(), [&](const auto &reference) {
                    return reference.observationOrder == observation.observationOrder;
                });
            QVERIFY(found != retry.evidence.cend());
            QCOMPARE(compat::bytes(found->directory + "/bytes"), observation.bytes);
            QCOMPARE(metadata(*found)["requestId"].toString(), observation.requestId);
        }
        QVERIFY(compat::bytes(path).contains("18446744073709551615"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        if (overlay) {
            const auto overrides =
                restart.value({"rows"}).toList().first().toMap()["instanceSettings"].toMap();
            QCOMPARE(D::leaf(T::Real, 0.0).validate(overrides["scale"]).value.toDouble(), 1.7);
            QVERIFY(overrides["opaque"].toBool());
        } else
            QCOMPARE(restart.value({"scale"}).toDouble(), 1.7);
    }
    void unreservedEnqueueEnforcesReleaseBound() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"scale\":1.0}");
        auto o = options(dir.path());
        auto blocked = std::make_shared<std::atomic_bool>(false);
        o.fileFault = [blocked](const QString &phase, const QString &) {
            return phase == "evidence-retain" && *blocked ? QString("blocked") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        *blocked = true;
        const auto limit = ConfigStoreTestAccess::evidenceBacklogLimit();
        QList<ConfigEvidenceObservation> before, after;
        QString error;
        control(store, [&](auto &worker) {
            for (qsizetype i = 0; i < limit; ++i)
                ConfigStoreTestAccess::acquireEvidence(worker, "json");
            before = ConfigStoreTestAccess::pendingEvidence(worker, "json");
            ConfigBackendResult unreserved;
            unreserved.success = unreserved.bytesAvailable = true;
            unreserved.observedPresence = true;
            unreserved.bytes = "not admitted past the bound";
            unreserved.revision = {true, "unreserved"};
            error = ConfigStoreTestAccess::retainUnreservedEvidence(worker, "json", unreserved);
            after = ConfigStoreTestAccess::pendingEvidence(worker, "json");
        });
        QCOMPARE(error, QString("evidence-backlog"));
        QCOMPARE(before.size(), limit);
        QCOMPARE(after.size(), limit);
        for (qsizetype i = 0; i < limit; ++i) {
            QCOMPARE(after[i].bytes, before[i].bytes);
            QCOMPARE(after[i].observationOrder, before[i].observationOrder);
        }
        *blocked = false;
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        control(store, [&](auto &worker) {
            QVERIFY(ConfigStoreTestAccess::pendingEvidence(worker, "json").isEmpty());
        });
        QCOMPARE(store.value({"scale"}).toDouble(), 1.0);
    }
    void saturatedFrozenPendingRebaseDrainsInReceiptOrder() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"scale\":1.0,\"wide\":18446744073709551615}";
        compat::put(path, original);
        auto o = options(dir.path());
        auto writeFault = std::make_shared<std::atomic_bool>(true);
        auto blocked = std::make_shared<std::atomic_bool>(false);
        o.fileFault = [=](const QString &phase, const QString &) {
            if (phase == "write" && writeFault->exchange(false))
                return QString("no-effect-write-fault");
            return phase == "evidence-retain" && *blocked ? QString("blocked") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"scale"}, 1.7).accepted);
        const auto frozen = compat::request(store, [&] { return store.save(); });
        QCOMPARE(frozen.error.code, QString("no-effect-write-fault"));
        QCOMPARE(compat::bytes(path), original);
        control(store, [&](auto &worker) {
            QVERIFY(ConfigStoreTestAccess::frozenIntents(worker, "json") > 0);
        });
        *blocked = true;
        const auto limit = ConfigStoreTestAccess::evidenceBacklogLimit();
        QList<QByteArray> bytes;
        QList<quint64> requests;
        for (qsizetype i = 0; i < limit; ++i) {
            bytes.append("frozen acquired malformed body " + QByteArray::number(i));
            replace(path, bytes.last());
            const auto result = compat::request(store, [&] { return store.reload(); });
            QCOMPARE(result.error.code,
                     i + 1 == limit ? QString("evidence-backlog") : QString("evidence-retention"));
            requests.append(result.requestId);
        }
        QList<ConfigEvidenceObservation> pending;
        control(store, [&](auto &worker) {
            pending = ConfigStoreTestAccess::pendingEvidence(worker, "json");
            QVERIFY(ConfigStoreTestAccess::frozenIntents(worker, "json") > 0);
        });
        QCOMPARE(pending.size(), limit);
        const auto refused = compat::request(store, [&] { return store.save(); });
        QCOMPARE(refused.error.code, QString("evidence-backlog"));
        QCOMPARE(compat::bytes(path), bytes.last());
        replace(path, original);
        *blocked = false;
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QVERIFY(saved.evidence.size() > limit);
        for (qsizetype i = 0; i < limit; ++i) {
            const auto &reference = saved.evidence[i];
            QCOMPARE(reference.observationOrder, pending[i].observationOrder);
            QCOMPARE(compat::bytes(reference.directory + "/bytes"), bytes[i]);
            const auto meta = metadata(reference);
            QCOMPARE(meta["requestId"].toString(), QString::number(requests[i]));
            QCOMPARE(meta["phase"].toString(), QString("read"));
            QCOMPARE(meta["role"].toString(), QString("foreign"));
            if (i)
                QVERIFY(saved.evidence[i - 1].observationOrder < reference.observationOrder);
        }
        for (qsizetype i = limit; i < saved.evidence.size(); ++i)
            QVERIFY(saved.evidence[i].observationOrder > pending.last().observationOrder);
        control(store, [&](auto &worker) {
            QVERIFY(ConfigStoreTestAccess::pendingEvidence(worker, "json").isEmpty());
            QCOMPARE(ConfigStoreTestAccess::frozenIntents(worker, "json"), qsizetype(0));
        });
        QVERIFY(compat::bytes(path).contains("18446744073709551615"));
        QCOMPARE(store.value({"scale"}).toDouble(), 1.7);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"scale"}).toDouble(), 1.7);
    }
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
