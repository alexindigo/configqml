// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "fileoptions.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSaveFile>
#include <QProcess>
#include <QSignalSpy>
#include <QDir>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
using namespace configqml;
namespace {
ConfigStoreOptions strictOptions(const QString &directory, const QString &path = {}) {
    auto o = fileOptions(path.isEmpty() ? directory + "/user.json" : path);
    o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictSourcePreimage;
    o.evidenceDirectory = directory + "/evidence";
    return o;
}
void replace(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        qFatal("fixture atomic replace");
}
QVariantMap metadata(const ConfigEvidenceReference &ref) {
    return decodeJsonValue(compat::bytes(ref.directory + "/metadata.json")).value.toMap();
}
ConfigRetainedGroupOptions group() { return {"strict-head", {{"b"}}, {{{"b"}, true, 9}}, {}}; }
} // namespace
class StrictConflictsTest : public QObject {
    Q_OBJECT
private slots:
    void declaration() {
        QTemporaryDir dir;
        auto o = strictOptions(dir.path());
        o.evidenceDirectory.clear();
        ConfigStore missing(o);
        QTRY_COMPARE(missing.status(), ConfigStore::Status::Error);
        QCOMPARE(missing.error(), QString("evidence-directory"));
    }
    void disjointForeignAndFrozenRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{ \"a\":1,\"b\":1,\"opaque\":18446744073709551615 }\n";
        compat::put(path, original);
        ConfigStore store(strictOptions(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] { return store.retainGroup(group()); });
        QCOMPARE(capture.status, QString("retained"));
        QProcess writer;
        writer.start(QCoreApplication::applicationDirPath() + "/file_writer",
                     {path, "a", "7", "manual"});
        QVERIFY(writer.waitForStarted());
        QVERIFY(writer.waitForReadyRead());
        QVERIFY(writer.readAllStandardOutput().contains("READY"));
        writer.write("GO\n");
        QVERIFY(writer.waitForFinished(10000));
        QCOMPARE(writer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(writer.exitCode(), 0);
        QVERIFY(writer.readAllStandardOutput().contains("saved"));
        QCOMPARE(writer.readAllStandardError(), QByteArray());
        const auto foreign = compat::bytes(path);
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(store.value({"b"}).toInt(), 9);
        const auto failed =
            compat::request(store, [&] { return store.saveGroup(capture.groupToken); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, QString("source-preimage-conflict"));
        QCOMPARE(compat::bytes(path), foreign);
        bool sawOriginal = false, sawForeign = false;
        QSet<QString> ids;
        for (const auto &ref : failed.evidence) {
            const auto meta = metadata(ref);
            QCOMPARE(meta.keys(),
                     (QStringList{"attemptId", "bytesState", "evidenceId", "groupToken",
                                  "observationOrder", "phase", "presence", "requestId", "revision",
                                  "role", "sourceId", "storeId"}));
            QCOMPARE(meta.value("requestId").toString(), QString::number(failed.requestId));
            QCOMPARE(meta.value("groupToken").toString(), capture.groupToken);
            QCOMPARE(meta.value("attemptId").toString(), failed.attemptId);
            QVERIFY(!ids.contains(ref.evidenceId));
            ids.insert(ref.evidenceId);
            struct stat s;
            QVERIFY(::stat(QFile::encodeName(ref.directory).constData(), &s) == 0);
            QCOMPARE(s.st_mode & 0777, mode_t(0700));
            QVERIFY(::stat(QFile::encodeName(ref.directory + "/metadata.json").constData(), &s) ==
                    0);
            QCOMPARE(s.st_mode & 0777, mode_t(0600));
            if (ref.role == "original") {
                QCOMPARE(compat::bytes(ref.directory + "/bytes"), original);
                sawOriginal = true;
            }
            if (ref.role == "foreign") {
                QCOMPARE(compat::bytes(ref.directory + "/bytes"), foreign);
                sawForeign = true;
            }
        }
        QVERIFY(sawOriginal && sawForeign);
        const auto retry =
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); });
        QCOMPARE(retry.error.code, QString("source-preimage-conflict"));
        QCOMPARE(retry.attemptNumber, quint64(2));
        QCOMPARE(compat::bytes(path), foreign);
        ConfigStore restarted(strictOptions(dir.path()));
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(restarted.value({"a"}).toInt(), 7);
        QCOMPARE(restarted.value({"b"}).toInt(), 1);
    }
    void inodeAndABA() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray bytes = "{\"a\":1,\"b\":1}";
        compat::put(path, bytes);
        ConfigStore store(strictOptions(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 8).accepted);
        replace(path, "{\"a\":2,\"b\":1}");
        replace(path, bytes);
        const auto result = compat::request(store, [&] { return store.save(); });
        QCOMPARE(result.error.code, QString("source-preimage-conflict"));
        QCOMPARE(compat::bytes(path), bytes);
        QCOMPARE(store.value({"b"}).toInt(), 8);
    }
    void configuredLinkAndParentIdentity_data() {
        QTest::addColumn<bool>("parent");
        QTest::newRow("file-link-ABA") << false;
        QTest::newRow("parent-link-ABA") << true;
    }
    void configuredLinkAndParentIdentity() {
        QFETCH(bool, parent);
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("target")));
        const auto target = dir.filePath("target/file.json");
        compat::put(target, "{\"a\":1}");
        const auto link = dir.filePath("link");
        const auto destination = parent ? dir.filePath("target") : target;
        QVERIFY(QFile::link(destination, link));
        ConfigStore store(strictOptions(dir.path(), parent ? link + "/file.json" : link));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 8).accepted);
        QVERIFY(QFile::rename(link, link + ".old"));
        QVERIFY(QFile::link(destination, link));
        const auto result = compat::request(store, [&] { return store.save(); });
        QCOMPARE(result.error.code, QString("source-preimage-conflict"));
        QCOMPARE(compat::bytes(target), QByteArray("{\"a\":1}"));
    }
    void parentDirectoryReplacement() {
        QTemporaryDir dir;
        const auto parent = dir.filePath("parent");
        QVERIFY(QDir().mkpath(parent));
        compat::put(parent + "/user.json", "{\"a\":1}");
        ConfigStore store(strictOptions(dir.path(), parent + "/user.json"));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 8).accepted);
        QVERIFY(QDir().rename(parent, parent + ".old"));
        QVERIFY(QDir().mkpath(parent));
        // Preserve the very same file inode; only its parent identity changes.
        QVERIFY(QFile::rename(parent + ".old/user.json", parent + "/user.json"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("source-preimage-conflict"));
    }
    void ownOutputAfterFault_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("own-publication-error") << 0;
        QTest::newRow("foreign-readback") << 1;
        QTest::newRow("missing-readback") << 2;
    }
    void ownOutputAfterFault() {
        QFETCH(int, mode);
        const bool foreign = mode != 0;
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = strictOptions(dir.path());
        o.observedPaths = {{"b"}};
        auto once = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [once, mode](const QString &phase, const QString &file) {
            if (phase != "readback" || !once->exchange(false))
                return QString();
            if (mode == 1)
                replace(file, "{\"a\":77,\"b\":77}");
            if (mode == 2 && !QFile::remove(file))
                qFatal("missing readback fixture");
            return QString("readback-fault");
        };
        ConfigStore store(o);
        QList<ConfigSourceAccepted> accepted;
        QList<QVariant> deploymentInput;
        connect(&store, &ConfigStore::sourceAccepted, this, [&](const auto &event) {
            accepted.append(event);
            if (event.origin == "external")
                deploymentInput.append(event.facts.first().value);
        });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] { return store.retainGroup(group()); });
        const auto failed =
            compat::request(store, [&] { return store.saveGroup(capture.groupToken); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.publicationState, QString("uncertain"));
        bool published = false, readback = false;
        for (const auto &ref : failed.evidence) {
            if (ref.role == "published") {
                QVERIFY(compat::bytes(ref.directory + "/bytes").contains("\"b\":9"));
                published = true;
            }
            if (ref.role == (foreign ? "late" : "readback")) {
                if (mode == 2) {
                    QCOMPARE(metadata(ref).value("presence").toString(), QString("absent"));
                    QVERIFY(!QFileInfo::exists(ref.directory + "/bytes"));
                } else
                    QVERIFY(compat::bytes(ref.directory + "/bytes").contains(foreign ? "77" : "9"));
                readback = true;
            }
        }
        QVERIFY(published && readback);
        // A late/readback inspector artifact is evidence, never a current
        // accepted-source choice or a deployment receipt, even after an effect.
        QCOMPARE(accepted.size(), 1);
        QCOMPARE(accepted.first().origin, QString("startup"));
        QVERIFY(deploymentInput.isEmpty());
        QCOMPARE(store.value({"b"}).toInt(), 9);
        const auto retry =
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); });
        QCOMPARE(retry.status, foreign ? QString("failed") : QString("saved"));
        if (foreign)
            QCOMPARE(retry.error.code, QString("source-preimage-conflict"));
        ConfigStore restart(strictOptions(dir.path()));
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"b"}).toInt(), mode == 2 ? 0 : foreign ? 77 : 9);
    }
    void lateObservationRetainedBeforeStaleReply() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1}");
        ConfigStore store(strictOptions(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.holdReplies(true); });
        replace(path, "{\"a\":7}");
        const auto reloadId = store.reload();
        bool workerRead = false;
        ConfigStoreTestAccess::run(store, [](auto &) {}, this, [&] { workerRead = true; });
        QTRY_VERIFY(workerRead);
        QVERIFY(store.setUser({"a"}, 9).accepted);
        QCOMPARE(store.value({"a"}).toInt(), 9);
        QSignalSpy results(&store, &ConfigStore::completed);
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.releaseReplies(1); });
        QTRY_VERIFY(!results.isEmpty());
        const auto result = qvariant_cast<ConfigResult>(results.first().first());
        QCOMPARE(result.requestId, reloadId);
        QCOMPARE(store.value({"a"}).toInt(), 9);
        QVERIFY(!result.evidence.isEmpty());
        QCOMPARE(compat::bytes(result.evidence.first().directory + "/bytes"),
                 QByteArray("{\"a\":7}"));
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.holdReplies(false); });
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(store.value({"a"}).toInt(), 9);
    }
    void retentionFailureAfterPublication() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1}");
        auto o = strictOptions(dir.path());
        const auto evidence = o.evidenceDirectory;
        o.fileFault = [evidence](const QString &phase, const QString &) {
            if (phase == "readback") {
                if (!QDir().rename(evidence, evidence + ".old"))
                    qFatal("evidence fixture rename");
                compat::put(evidence, "not a directory");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        const auto result = compat::request(store, [&] { return store.save(); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("evidence-retention"));
        QVERIFY(!result.publications.isEmpty());
        QVERIFY(result.publications.last().success);
        QVERIFY(result.coveredRevisions.contains("json"));
        QVERIFY(compat::bytes(path).contains("\"b\":9"));
    }
    void retentionFailureSurvivesLaterPublicationFailure_data() {
        QTest::addColumn<bool>("retentionFailure");
        QTest::addColumn<bool>("uncertain");
        QTest::newRow("retention-then-known-failure") << true << false;
        QTest::newRow("retention-then-uncertain-failure") << true << true;
        QTest::newRow("ordinary-errors-still-select-last-known") << false << false;
        QTest::newRow("ordinary-errors-still-select-last-uncertain") << false << true;
    }
    void retentionFailureSurvivesLaterPublicationFailure() {
        QFETCH(bool, retentionFailure);
        QFETCH(bool, uncertain);
        using D = ConfigDescriptor;
        QTemporaryDir dir;
        const auto path = dir.filePath("a.json");
        const QByteArray original = "{\"n\":1}";
        compat::put(path, original);
        auto o = strictOptions(dir.path(), path);
        o.schema =
            ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(D::Type::SafeInteger, 0)}})},
                                   {"b", D::group({{"n", D::leaf(D::Type::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {{"n", 2}}}};
        o.jsonFiles[0].sourceId = "a";
        const auto evidence = o.evidenceDirectory;
        o.fileFault = [evidence, retentionFailure](const QString &phase, const QString &) {
            if (!retentionFailure && phase == "write")
                return QString("first-publication-fault");
            if (retentionFailure && phase == "readback") {
                if (!QDir().rename(evidence, evidence + ".retained"))
                    qFatal("evidence fixture rename");
                compat::put(evidence, "not a directory");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        bool scheduled = false;
        ConfigStoreTestAccess::run(
            store,
            [uncertain](auto &worker) {
                worker.scheduleBackend({false, uncertain
                                                   ? ConfigMemoryBackend::Fault::Readback
                                                   : ConfigMemoryBackend::Fault::BeforeOperation});
            },
            this, [&] { scheduled = true; });
        QTRY_VERIFY(scheduled);
        QVERIFY(store.setUser({"a", "n"}, 9).accepted);
        QVERIFY(store.setUser({"b", "n"}, 8).accepted);
        const auto result = compat::request(store, [&] { return store.save(); });
        const auto laterError =
            uncertain ? QString("injected-readback") : QString("injected-before-operation");
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, retentionFailure ? QString("evidence-retention") : laterError);
        QCOMPARE(result.error.sourceId, retentionFailure ? QString("a") : QString("b"));
        QCOMPARE(result.publications.size(), 2);
        const auto first = result.publications[0], second = result.publications[1];
        QCOMPARE(first.sourceId, QString("a"));
        QCOMPARE(first.success, retentionFailure);
        QCOMPARE(first.state, QString("known"));
        QCOMPARE(first.error.code,
                 retentionFailure ? QString() : QString("first-publication-fault"));
        QCOMPARE(second.sourceId, QString("b"));
        QVERIFY(!second.success);
        QCOMPARE(second.state, uncertain ? QString("uncertain") : QString("known"));
        QCOMPARE(second.error.code, laterError);
        QCOMPARE(second.error.sourceId, QString("b"));
        QCOMPARE(result.coveredRevisions.size(), retentionFailure ? 1 : 0);
        QVERIFY(!result.coveredRevisions.contains("b"));
        QCOMPARE(result.publicationState, retentionFailure ? QString("partial")
                                          : uncertain      ? QString("uncertain")
                                                           : QString("known"));
        if (retentionFailure) {
            QCOMPARE(result.coveredRevisions.value("a"), first.revision);
            ConfigJsonBackend reader(o.jsonFiles);
            const auto id = reader.submit({ConfigBackendRequest::Operation::Read, "a", {}, {}});
            const auto readback = reader.take(id);
            QVERIFY(readback->success);
            QCOMPARE(readback->revision, first.revision);
            QVERIFY(compat::bytes(path).contains("\"n\":9"));
            QVERIFY(QFileInfo(evidence + ".retained").isDir());
        } else
            QCOMPARE(compat::bytes(path), original);
        int memoryValue = -1;
        bool read = false;
        ConfigStoreTestAccess::run(
            store,
            [&](auto &worker) {
                const auto id =
                    worker.backend()->submit({ConfigBackendRequest::Operation::Read, "b", {}, {}});
                memoryValue =
                    decodeMemoryValue(worker.backend()->take(id)->bytes).value("n").toInt();
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QCOMPARE(memoryValue, uncertain ? 8 : 2);
    }
    void freshAbsentParentsAndOwnChain() {
        QTemporaryDir dir;
        ConfigStore store(strictOptions(dir.path(), dir.filePath("new/deep/user.json")));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"a"}, 1).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(store.setUser({"b"}, 2).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
    }
    void ownWitnessAdvancesDependentStrictCaptures() {
        QTemporaryDir dir;
        compat::put(dir.filePath("user.json"), "{\"a\":1,\"b\":1}");
        ConfigStore store(strictOptions(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto first = compat::request(store, [&] { return store.retainGroup(group()); });
        auto suffix = group();
        suffix.requestKey = "suffix";
        suffix.dependencies = {first.groupToken};
        suffix.edits[0].value = 11;
        const auto second = compat::request(store, [&] { return store.retainGroup(suffix); });
        QCOMPARE(second.status, QString("retained"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(first.groupToken); }).status,
                 QString("saved"));
        QCOMPARE(store.value({"b"}).toInt(), 11);
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(second.groupToken); }).status,
                 QString("saved"));
        ConfigStore restart(strictOptions(dir.path()));
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"b"}).toInt(), 11);
    }
};
QTEST_GUILESS_MAIN(StrictConflictsTest)
#include "test_strict_conflicts.moc"
