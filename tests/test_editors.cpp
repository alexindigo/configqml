// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configstorehandle.h"
#include "storeprobe.h"
#include "modelbudgets.h"
#include "confignumber.h"
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>
#include <QSemaphore>
#include <QDir>
#include <QProcess>
#include <sys/stat.h>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;

static void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("fixture write failed");
}
static QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("fixture read failed");
    return file.readAll();
}
static ConfigResult request(ConfigStore &store, const std::function<quint64()> &dispatch) {
    QEventLoop loop;
    ConfigResult result;
    quint64 id = 0;
    const auto connection =
        QObject::connect(&store, &ConfigStore::completed, &loop, [&](const ConfigResult &r) {
            if (r.requestId == id) {
                result = r;
                loop.quit();
            }
        });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    id = dispatch();
    loop.exec();
    QObject::disconnect(connection);
    if (!result.requestId)
        qFatal("request timeout");
    return result;
}
static ConfigStoreOptions options(const QString &directory = {}) {
    auto secret = D::leaf(T::String, QString("private-default"));
    secret.sensitive = true;
    auto local = D::leaf(T::String, QString("local-default"));
    local.machineLocal = true;
    auto record = D::leaf(T::Record, QVariantMap{{"title", QString("default")}});
    record.fields = {{"title", D::leaf(T::String, QString())}};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(
        D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)},
                                  {"secret", secret},
                                  {"local", local},
                                  {"reference", D::leaf(T::SecretRef, QString("keyring:item"))},
                                  {"record", record},
                                  {"rows", D::list(D::leaf(T::SafeInteger, 0), {})}})},
                  {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"a", {"a"}, {}}, {"b", {"b"}, {}}},
                 true}};
    if (!directory.isEmpty())
        o.jsonFiles = {{"user", "a", directory + "/a.json"}, {"user", "b", directory + "/b.json"}};
    return o;
}
class EditorsTest : public QObject {
    Q_OBJECT
private slots:
    void detachedCoherentAndReopened() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        QVERIFY(draft->valid());
        QVERIFY(draft->setValue({"a", "n"}, 7).accepted);
        QVERIFY(draft->setValue({"b", "n"}, 8).accepted);
        QCOMPARE(store.value({"a", "n"}).toInt(), 0);
        QCOMPARE(draft->value({"a", "n"}).toInt(), 7);
        QVERIFY(draft->dirty());
        int changes = 0;
        connect(&store, &ConfigStore::valuesChanged, this, [&](const auto &) {
            ++changes;
            QCOMPARE(store.value({"a", "n"}).toInt(), 7);
            QCOMPARE(store.value({"b", "n"}).toInt(), 8);
        });
        bool callback = false;
        auto result = request(store, [&] {
            return draft->apply(this, [&](const auto &) {
                QVERIFY(!draft->dirty());
                callback = true;
            });
        });
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(result.publications.size(), 2);
        QCOMPARE(changes, 1);
        QVERIFY(callback);
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"a", "n"}).toInt(), 7);
        QCOMPARE(reopened.value({"b", "n"}).toInt(), 8);
        QVERIFY(draft->setValue({"a", "n"}, 9).accepted);
        QVERIFY(draft->discard());
        QCOMPARE(draft->value({"a", "n"}).toInt(), 7);
        QVERIFY(!draft->dirty());
    }
    void externalConflictsAndDisjointPaths() {
        QTemporaryDir dir;
        writeFile(dir.filePath("a.json"), "{\"n\":1,\"local\":\"first\"}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"a", "n"}});
        QVERIFY(draft->setValue({"a", "n"}, 7).accepted);
        writeFile(dir.filePath("a.json"), "{\"n\":1,\"local\":\"second\"}");
        request(store, [&] { return store.reload(); });
        QVERIFY(!draft->conflicted());
        writeFile(dir.filePath("a.json"), "{\"n\":2,\"local\":\"second\"}");
        request(store, [&] { return store.reload(); });
        QVERIFY(draft->conflicted());
        QCOMPARE(draft->value({"a", "n"}).toInt(), 7);
        const auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.error.code, QString("draft-conflict"));
        QVERIFY(draft->dirty());
        QCOMPARE(store.value({"a", "n"}).toInt(), 2);
        QVERIFY(draft->discard());
        QCOMPARE(draft->value({"a", "n"}).toInt(), 2);
        QVERIFY(!draft->conflicted());
        QVERIFY(draft->setValue({"a", "n"}, 4).accepted);
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
    }
    void queuedFenceAndAtomicRejection() {
        auto o = options();
        o.admission = [](const ConfigInput &input) {
            return input.path == ConfigPath{"b", "n"} ? QString("policy") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        draft->setValue({"a", "n"}, 7);
        draft->setValue({"b", "n"}, 8);
        auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.error.code, QString("policy"));
        QCOMPARE(store.value({"a", "n"}).toInt(), 0);
        QCOMPARE(store.value({"b", "n"}).toInt(), 0);
        QVERIFY(draft->dirty());
        QVERIFY(draft->discard());
        draft->setValue({"a", "n"}, 9);
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.holdInputs(true); });
        store.setUser({"a", "n"}, 3);
        const auto id = draft->apply(); // Worker must fence the not-yet-published preceding write.
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.holdInputs(false); });
        QSignalSpy results(&store, &ConfigStore::completed);
        QTRY_VERIFY(!results.isEmpty());
        QTRY_COMPARE(results.last().first().value<ConfigResult>().requestId, id);
        QCOMPARE(results.last().first().value<ConfigResult>().error.code,
                 QString("draft-conflict"));
        QCOMPARE(draft->value({"a", "n"}).toInt(), 9);
        QCOMPARE(store.value({"a", "n"}).toInt(), 3);
    }
    void rejectedDraftRetry_data() {
        QTest::addColumn<int>("intervening");
        QTest::newRow("publication") << 0;
        QTest::newRow("disjoint-intent") << 1;
        QTest::newRow("equal-same-path-intent") << 2;
        QTest::newRow("newer-local-input") << 3;
    }
    void rejectedDraftRetry() {
        QFETCH(int, intervening);
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.admission = [](const ConfigInput &input) {
            return input.value.toInt() == 7 ? QString("policy") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"a", "n"}});
        QVERIFY(draft->setValue({"a", "n"}, 7).accepted);
        QSignalSpy results(&store, &ConfigStore::completed);
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.holdReplies(true); });
        const auto rejected = draft->apply();
        if (intervening == 3)
            ConfigStoreTestAccess::run(store, [](auto &worker) { worker.holdInputs(true); });
        if (intervening == 0)
            store.reload();
        else
            store.setUser(intervening == 1 ? ConfigPath{"b", "n"} : ConfigPath{"a", "n"}, 0);
        ConfigStoreTestAccess::run(store, [](auto &worker) {
            worker.holdReplies(false);
            worker.holdInputs(false);
        });
        QTRY_COMPARE(results.size(), 2);
        QCOMPARE(results[0][0].value<ConfigResult>().requestId, rejected);
        QCOMPARE(results[0][0].value<ConfigResult>().error.code, QString("policy"));
        QVERIFY(draft->dirty());
        QCOMPARE(draft->value({"a", "n"}).toInt(), 7);
        QVERIFY(draft->setValue({"a", "n"}, 8).accepted);
        const auto retry = request(store, [&] { return draft->apply(); });
        if (intervening <= 1) {
            QCOMPARE(retry.status, QString("saved"));
            QVERIFY(!draft->dirty());
            ConfigStore reopened(o);
            QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
            QCOMPARE(reopened.value({"a", "n"}).toInt(), 8);
        } else {
            QCOMPARE(retry.error.code, QString("draft-conflict"));
            QVERIFY(draft->conflicted());
            QVERIFY(draft->dirty());
        }
    }
    void correctedDraftAfterRejection() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.admission = [](const ConfigInput &input) {
            return input.value.toInt() == 7 ? QString("policy") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"a", "n"}});
        draft->setValue({"a", "n"}, 7);
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code, QString("policy"));
        QVERIFY(draft->dirty());
        QCOMPARE(draft->value({"a", "n"}).toInt(), 7);
        QCOMPARE(request(store, [&] { return store.reload(); }).status, QString("reloaded"));
        QVERIFY(!draft->conflicted());
        draft->setValue({"a", "n"}, 8);
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.value({"a", "n"}).toInt(), 8);
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"a", "n"}).toInt(), 8);
    }
    void partialFailureRetainsDraft() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.fileFault = [](const QString &phase, const QString &file) {
            return file.endsWith("b.json") && phase == "write" ? QString("injected-stage")
                                                               : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        draft->setValue({"a", "n"}, 7);
        draft->setValue({"b", "n"}, 8);
        const auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("partial"));
        QCOMPARE(result.publications.size(), 2);
        QVERIFY(draft->dirty());
        QCOMPARE(draft->value({"b", "n"}).toInt(), 8);
        const auto acceptedIntent = store.info({"b", "n"}).intentRevision;
        QVERIFY(acceptedIntent > 0);
        QVERIFY(result.rejectedPathGenerations.isEmpty());
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                 QString("draft-conflict"));
        QVERIFY(draft->dirty() && draft->conflicted());
        QCOMPARE(store.info({"b", "n"}).intentRevision, acceptedIntent);
        QVERIFY(!store.failures().isEmpty());
        o.fileFault = {};
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"a", "n"}).toInt(), 7);
        QCOMPARE(reopened.value({"b", "n"}).toInt(), 0);
    }
    void scopesOpaqueAndIndependentDelivery() {
        auto o = options();
        const auto leaf = D::group({{"n", D::leaf(T::SafeInteger, 0)}});
        o.schema =
            ConfigSchema(D::group({{"plugins", D::group({{"../a.b_雪", leaf}, {"other", leaf}})}}));
        o.layers[0].sources = {{"first", {"plugins", "../a.b_雪"}, {}},
                               {"second", {"plugins", "other"}, {}}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *first = store.scope({"plugins", "../a.b_雪"});
        auto *second = store.scope({"plugins", "other"});
        auto *readOnly = first->scope({}, false);
        QVERIFY(!readOnly->setUser({"n"}, 9).accepted);
        QVERIFY(!readOnly->scope({}, true)->setUser({"n"}, 9).accepted);
        bool cancelled = true, delivered = false;
        first->setUser({"n"}, 7, [&](const auto &) { cancelled = false; });
        first->detach();
        second->setUser({"n"}, 8, [&](const auto &) { delivered = true; });
        QCOMPARE(request(store, [&] { return second->save(); }).status, QString("saved"));
        QVERIFY(cancelled && delivered);
        QCOMPARE(second->value({"n"}).toInt(), 8);
        QCOMPARE(store.value({"plugins", "../a.b_雪", "n"}).toInt(), 7);
        QCOMPARE(second->info({"n"}).provenance.route.sourceId, QString("second"));
    }
    void profileImportLimits_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"oversized", "fifo", "directory", "device"})
            QTest::newRow(kind) << QString(kind);
    }
    void profileImportLimits() {
        QFETCH(QString, kind);
        QTemporaryDir dir;
        auto path = dir.filePath("input.json");
        if (kind == "oversized") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(16 * 1024 * 1024 + 1));
        } else if (kind == "fifo")
            QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        else if (kind == "directory")
            path = dir.path();
        else
            path = "/dev/zero";
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QElapsedTimer timer;
        timer.start();
        const auto result = request(store, [&] { return store.importProfile(path); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code,
                 kind == "oversized" ? QString("profile-too-large") : QString("profile-file-type"));
        QVERIFY(timer.elapsed() < 1000);
        QCOMPARE(store.value({"a", "n"}).toInt(), 0);
        // A refused import must settle its prefix and leave the worker responsive.
        QCOMPARE(request(store, [&] { return store.close(); }).status, QString("nothing_to_save"));
    }
    void configuredProfileByteLimit() {
        QTemporaryDir dir;
        const auto path = dir.filePath("profile.json");
        const auto bytes =
            QByteArray("{\"format\":1,\"name\":\"Bounded\",\"entries\":[{\"path\":[\"a\",\"n\"],"
                       "\"present\":true,\"value\":7}]}") +
            QByteArray(70000, ' ');
        writeFile(path, bytes);
        auto o = options();
        o.profileMaxBytes = bytes.size();
        ConfigStore exact(o);
        QTRY_COMPARE(exact.status(), ConfigStore::Status::Ready);
        QCOMPARE(request(exact, [&] { return exact.importProfile(path); }).status,
                 QString("saved"));
        QCOMPARE(exact.value({"a", "n"}).toInt(), 7);
        --o.profileMaxBytes;
        ConfigStore over(o);
        QTRY_COMPARE(over.status(), ConfigStore::Status::Ready);
        QCOMPARE(request(over, [&] { return over.importProfile(path); }).error.code,
                 QString("profile-too-large"));
        QCOMPARE(over.value({"a", "n"}).toInt(), 0);
        o.profileMaxBytes = 0;
        ConfigStore invalid(o);
        QTRY_COMPARE(invalid.status(), ConfigStore::Status::Ready);
        QCOMPARE(request(invalid, [&] { return invalid.importProfile(path); }).error.code,
                 QString("profile-size-limit"));
    }
    void rawProfileRoundTripAndExclusions() {
        QTemporaryDir dir;
        writeFile(dir.filePath("a.json"),
                  "{\"n\":4,\"secret\":\"synthetic-private\",\"local\":\"device\","
                  "\"reference\":\"keyring:synthetic\",\"unknown\":\"synthetic-legacy\","
                  "\"record\":{\"title\":\"portable\",\"legacyToken\":\"synthetic-nested\"}}");
        writeFile(dir.filePath("b.json"), "{\"n\":5}");
        auto o = options(dir.path());
        // A runtime mask must never be exported instead of the raw user override.
        o.layers.append({"runtime",
                         ConfigLayerKind::Runtime,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"runtime", {}, {{"a", QVariantMap{{"n", 99}}}}}},
                         false});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto profile = dir.filePath("portable.json");
        QCOMPARE(request(store, [&] { return store.exportProfile("Portable", profile); }).status,
                 QString("exported"));
        const auto bytes = readFile(profile);
        QVERIFY(!bytes.contains("synthetic-"));
        QVERIFY(!bytes.contains("device"));
        QVERIFY(!bytes.contains("keyring:"));
        QVERIFY(!bytes.contains("legacyToken"));
        QVERIFY(!bytes.contains("99"));
        store.setUser({"a", "n"}, 20);
        store.setUser({"b", "n"}, 21);
        request(store, [&] { return store.save(); });
        auto result = request(store, [&] { return store.importProfile(profile); });
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(result.publications.size(), 2);
        QCOMPARE(store.value({"a", "n"}).toInt(), 99);
        QCOMPARE(store.value({"b", "n"}).toInt(), 5);
        QCOMPARE(store.value({"a", "secret"}).toString(), QString("synthetic-private"));
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"b", "n"}).toInt(), 5);
        const auto raw = ConfigJsonCodec().decode(readFile(dir.filePath("a.json")), "a").raw;
        QCOMPARE(raw.value("n").value<ConfigNumber>().token(), QString("4"));
        QCOMPARE(raw.value("unknown").toString(), QString("synthetic-legacy"));
        const auto selected = dir.filePath("selected.json");
        request(store, [&] { return store.exportProfile("A", selected, {{"a", "n"}}); });
        store.setUser({"b", "n"}, 42);
        request(store, [&] { return store.save(); });
        request(store, [&] { return store.importProfile(selected); });
        QCOMPARE(store.value({"b", "n"}).toInt(), 42);
    }
    void migrationsPersistAndPreserveOriginals() {
        QTemporaryDir dir;
        const QByteArray original = "{\"old\":7,\"unknown\":18446744073709551615}";
        writeFile(dir.filePath("a.json"), original);
        auto o = options(dir.path());
        auto workerThread = std::make_shared<std::atomic_bool>(false);
        const auto objectThread = QThread::currentThread();
        o.schema = ConfigSchema(*o.schema.descriptor({}), 2,
                                {{0,
                                  [workerThread, objectThread](const QVariantMap &input) {
                                      *workerThread = QThread::currentThread() != objectThread;
                                      auto raw = input;
                                      raw["n"] = raw.take("old");
                                      return ConfigValueResult{raw, {}};
                                  }},
                                 {1, [](const QVariantMap &input) {
                                      auto raw = input;
                                      raw["added"] = true;
                                      return ConfigValueResult{raw, {}};
                                  }}});
        ConfigStore store(o);
        QSignalSpy migrated(&store, &ConfigStore::migrationCompleted);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(workerThread->load());
        QCOMPARE(store.value({"a", "n"}).toInt(), 7);
        QCOMPARE(migrated.size(), 1);
        const auto result = migrated.first().first().value<ConfigResult>();
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(readFile(dir.filePath("a.json.configqml-v0.bak")), original);
        const auto raw = ConfigJsonCodec().decode(readFile(dir.filePath("a.json")), "a").raw;
        QVERIFY(raw.value("added").toBool());
        QCOMPARE(raw.value("unknown").value<ConfigNumber>().token(),
                 QString("18446744073709551615"));
        QCOMPARE(raw.value("_configqmlVersion").value<ConfigNumber>().token(), QString("2"));
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"a", "n"}).toInt(), 7);
    }
    void privateBackupAtCreation_data() {
        QTest::addColumn<bool>("reload");
        QTest::newRow("startup-symlink") << false;
        QTest::newRow("reload-symlink") << true;
    }
    void privateBackupAtCreation() {
        QFETCH(bool, reload);
        QTemporaryDir dir;
        QVERIFY(QFile::setPermissions(
            dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup |
                            QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
        const QByteArray original = "{\"old\":7,\"synthetic-private\":true}";
        const auto target = dir.filePath("target.json");
        const auto configured = dir.filePath("a.json");
        writeFile(target, reload ? QByteArray("{\"n\":1,\"_configqmlVersion\":1}") : original);
        QVERIFY(QFile::link(target, configured));
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1, {{0, [](const QVariantMap &input) {
                                                            auto raw = input;
                                                            raw["n"] = raw.take("old");
                                                            return ConfigValueResult{raw, {}};
                                                        }}});
        auto entered = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        auto stage = std::make_shared<QString>();
        o.fileFault = [entered, release, stage](const QString &phase, const QString &path) {
            if (phase == "backup-created") {
                *stage = path;
                entered->release();
                if (!release->tryAcquire(1, 10000))
                    return QString("test-timeout");
            }
            return QString();
        };
        const auto mask = ::umask(0022);
        ConfigStore store(o);
        if (reload) {
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            writeFile(target, original);
            store.reload();
        }
        const bool paused = entered->tryAcquire(1, 5000);
        ::umask(mask);
        if (!paused)
            release->release();
        QVERIFY(paused);
        struct stat state{};
        const auto statResult = ::stat(QFile::encodeName(*stage).constData(), &state);
        // Actually open as another UID, with a readable source as the traversal control.
        const auto otherUidRead = [](const QString &file) {
            QProcess child;
            // Enter the fixture before dropping privileges: a tester's private home
            // must not make the source traversal control a false permission denial.
            child.start("sudo", {"-n", "--", "python3", "-c",
                                 "import os,sys; os.chdir(sys.argv[1]); os.setgroups([]); "
                                 "os.setgid(65534); os.setuid(65534); "
                                 "os.close(os.open(sys.argv[2], os.O_RDONLY))",
                                 QFileInfo(file).absolutePath(), QFileInfo(file).fileName()});
            if (!child.waitForStarted(5000))
                return -1;
            if (!child.waitForFinished(5000)) {
                child.kill();
                child.waitForFinished(5000);
                return -1;
            }
            return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : -1;
        };
        const auto sourceRead = otherUidRead(target);
        const auto privateRead = sourceRead == 0 ? otherUidRead(*stage) : -1;
        const auto empty = readFile(*stage).isEmpty();
        const auto finalAbsent = !QFile::exists(target + ".configqml-v0.bak");
        release->release(); // Always release before assertions, including failure paths.
        QCOMPARE(statResult, 0);
        QCOMPARE(state.st_mode & 0777, mode_t(0600));
        QVERIFY(empty);
        QVERIFY(finalAbsent);
        QTRY_VERIFY(QFile::exists(target + ".configqml-v0.bak"));
        QCOMPARE(readFile(target + ".configqml-v0.bak"), original);
        QTRY_COMPARE(store.value({"a", "n"}).toInt(), 7);
        QVERIFY(QFileInfo(configured).isSymLink());
        QVERIFY(!QFile::exists(configured + ".configqml-v0.bak"));
        if (sourceRead != 0 || privateRead < 0)
            QSKIP("Other-user read probe unavailable: requires runnable sudo/python3 and "
                  "passwordless sudo; 0600 and completed migration were checked.");
        QCOMPARE(privateRead, 1);
    }
    void backupFailuresAndDurability_data() {
        QTest::addColumn<QString>("phase");
        QTest::addColumn<bool>("reload");
        for (const auto &phase : {"backup-create", "backup-write", "backup-flush",
                                  "backup-file-sync", "backup-publish", "backup-directory-sync"})
            for (bool reload : {false, true})
                QTest::newRow((QByteArray(phase) + (reload ? "-reload" : "-startup")).constData())
                    << QString(phase) << reload;
    }
    void backupFailuresAndDurability() {
        QFETCH(QString, phase);
        QFETCH(bool, reload);
        QTemporaryDir dir;
        const QByteArray original = "{\"old\":7,\"padding\":\"" + QByteArray(40000, 'x') + "\"}";
        const auto file = dir.filePath("a.json");
        const auto backup = file + ".configqml-v0.bak";
        writeFile(file, reload ? QByteArray("{\"n\":1,\"_configqmlVersion\":1}") : original);
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1, {{0, [](const QVariantMap &input) {
                                                            auto raw = input;
                                                            raw["n"] = raw.take("old");
                                                            return ConfigValueResult{raw, {}};
                                                        }}});
        auto calls = std::make_shared<int>(0);
        auto stage = std::make_shared<QString>();
        auto partial = std::make_shared<bool>(false);
        o.fileFault = [phase, calls, stage, partial](const QString &step, const QString &path) {
            if (step == "backup-created")
                *stage = path;
            if (step != phase)
                return QString();
            // Fail after a written prefix, exercising removal of incomplete staging.
            if (step == "backup-write" && ++*calls <= 2)
                return QString();
            if (step == "backup-write") {
                const auto prefix = readFile(*stage);
                *partial = !prefix.isEmpty() && prefix.size() < 40000;
            }
            return QString("injected-") + phase;
        };
        {
            ConfigStore store(o);
            QSignalSpy results(&store, &ConfigStore::migrationCompleted);
            if (reload) {
                QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
                results.clear();
                writeFile(file, original);
                QCOMPARE(request(store, [&] { return store.reload(); }).status, QString("failed"));
                QCOMPARE(store.value({"a", "n"}).toInt(), 1);
            } else
                QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
            QCOMPARE(results.size(), 1);
            QCOMPARE(results[0][0].value<ConfigResult>().error.code, "injected-" + phase);
            QCOMPARE(readFile(file), original);
            if (phase == "backup-write")
                QVERIFY(*partial);
            if (phase == "backup-directory-sync")
                QCOMPARE(readFile(backup), original); // Complete published evidence is retained.
            else
                QVERIFY(!QFile::exists(backup));
            QCOMPARE(QDir(dir.path()).entryList({"*.stage"}, QDir::Files | QDir::Hidden).size(), 0);
        }
        o.fileFault = {};
        ConfigStore retry(o);
        QTRY_COMPARE(retry.status(), ConfigStore::Status::Ready);
        QCOMPARE(retry.value({"a", "n"}).toInt(), 7);
        QCOMPARE(readFile(backup), original);
    }
    void backupOrderingAndExistingEvidence_data() {
        QTest::addColumn<int>("existing");
        QTest::newRow("new") << 0;
        QTest::newRow("reused") << 1;
        QTest::newRow("conflict") << 2;
        QTest::newRow("publication-race") << 3;
        QTest::newRow("reuse-sync-failure") << 4;
        QTest::newRow("reuse-file-sync-failure") << 5;
    }
    void backupOrderingAndExistingEvidence() {
        QFETCH(int, existing);
        QTemporaryDir dir;
        const auto file = dir.filePath("a.json");
        const auto backup = file + ".configqml-v0.bak";
        const QByteArray original = "{\"n\":7}";
        const QByteArray conflict = "preserve-existing-evidence";
        writeFile(file, original);
        if (existing == 1 || existing == 2 || existing >= 4) {
            writeFile(backup, existing == 2 ? conflict : original);
            QVERIFY(QFile::setPermissions(backup, QFile::ReadOwner | QFile::WriteOwner));
        }
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1,
                         {{0, [](const QVariantMap &raw) { return ConfigValueResult{raw, {}}; }}});
        auto trace = std::make_shared<QStringList>();
        o.fileFault = [trace, backup, conflict, existing](const QString &phase, const QString &) {
            trace->append(phase);
            if (existing == 3 && phase == "backup-publish")
                writeFile(backup, conflict);
            if (existing == 4 && phase == "backup-directory-sync")
                return QString("injected-reuse-sync");
            if (existing == 5 && phase == "backup-file-sync")
                return QString("injected-reuse-sync");
            return QString();
        };
        ConfigStore store(o);
        QSignalSpy results(&store, &ConfigStore::migrationCompleted);
        QTRY_VERIFY(store.status() != ConfigStore::Status::Loading);
        QCOMPARE(results.size(), 1);
        if (existing >= 2) {
            QCOMPARE(store.status(), ConfigStore::Status::Error);
            QCOMPARE(readFile(file), original);
            QCOMPARE(readFile(backup), existing >= 4 ? original : conflict);
            QCOMPARE(results[0][0].value<ConfigResult>().error.code,
                     existing >= 4 ? QString("injected-reuse-sync")
                                   : QString("migration-backup-conflict"));
            QVERIFY(!trace->contains("rename"));
        } else {
            QCOMPARE(store.status(), ConfigStore::Status::Ready);
            QCOMPARE(readFile(backup), original);
            const auto sync = trace->indexOf("backup-file-sync");
            const auto directory = trace->indexOf("backup-directory-sync");
            const auto rename = trace->indexOf("rename");
            QVERIFY(sync >= 0 && directory > sync && rename > directory);
            if (existing == 0) {
                QVERIFY(trace->indexOf("backup-flush") < sync);
                QVERIFY(trace->indexOf("backup-publish") > sync);
                QVERIFY(trace->indexOf("backup-publish") < directory);
            }
        }
    }
    void backupCleanupFailureReportsOwnedStaging() {
        QTemporaryDir dir;
        const auto file = dir.filePath("a.json");
        const auto backup = file + ".configqml-v0.bak";
        const QByteArray original = "{\"n\":7}";
        writeFile(file, original);
        const auto orphan = dir.filePath(".unrelated.stage");
        writeFile(orphan, "pre-existing-staging-evidence");
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1,
                         {{0, [](const QVariantMap &raw) { return ConfigValueResult{raw, {}}; }}});
        o.fileFault = [](const QString &phase, const QString &) {
            return phase == "backup-flush" || phase == "backup-cleanup" ? "injected-" + phase
                                                                        : QString();
        };
        {
            ConfigStore store(o);
            QSignalSpy results(&store, &ConfigStore::migrationCompleted);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
            QCOMPARE(results.size(), 1);
            const auto error = results[0][0].value<ConfigResult>().error;
            QCOMPARE(error.code, QString("injected-backup-cleanup"));
            QCOMPARE(error.operation, QString("backup-cleanup"));
            QVERIFY(error.file.endsWith(".stage"));
            QVERIFY(error.file != orphan);
            QVERIFY(QFile::exists(error.file));
            QCOMPARE(readFile(file), original);
            QVERIFY(!QFile::exists(backup));
        }
        o.fileFault = {};
        ConfigStore retry(o);
        QTRY_COMPARE(retry.status(), ConfigStore::Status::Ready);
        QCOMPARE(readFile(backup), original);
        QCOMPARE(readFile(orphan), QByteArray("pre-existing-staging-evidence"));
    }
    void migrationFailureKeepsOriginal_data() {
        QTest::addColumn<bool>("ioFailure");
        QTest::newRow("transform") << false;
        QTest::newRow("publication") << true;
    }
    void migrationFailureKeepsOriginal() {
        QFETCH(bool, ioFailure);
        QTemporaryDir dir;
        const QByteArray original = "{\"old\":7,\"unknown\":true}";
        writeFile(dir.filePath("a.json"), original);
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1,
                         {{0, [ioFailure](const QVariantMap &raw) {
                               return ioFailure
                                          ? ConfigValueResult{raw, {}}
                                          : ConfigValueResult{{}, {{{}, "synthetic-transform"}}};
                           }}});
        if (ioFailure)
            o.fileFault = [](const QString &phase, const QString &) {
                return phase == "write" ? QString("synthetic-stage") : QString();
            };
        ConfigStore store(o);
        QSignalSpy result(&store, &ConfigStore::migrationCompleted);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QCOMPARE(readFile(dir.filePath("a.json")), original);
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().first().value<ConfigResult>().status, QString("failed"));
    }
    void perNodeAdmissionAndRedaction() {
        auto tolerant = D::leaf(T::SafeInteger, 3);
        tolerant.admissionPolicy = D::AdmissionPolicy::Tolerant;
        auto strict = D::leaf(T::SafeInteger, 4);
        strict.admissionPolicy = D::AdmissionPolicy::Strict;
        auto o = options();
        o.schema = ConfigSchema(D::group({{"tolerant", tolerant}, {"strict", strict}}));
        o.layers[0].sources = {
            {"all", {}, {{"tolerant", QString("synthetic-invalid")}, {"strict", 8}}}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"tolerant"}).toInt(), 3);
        QCOMPARE(store.value({"strict"}).toInt(), 8);
        const auto info = store.info({"tolerant"});
        QVERIFY(info.provenance.contributions.last().rawPresent);
        QVERIFY(!info.provenance.contributions.last().valid);
        o.layers[0].sources[0].raw["strict"] = QString("synthetic-invalid");
        ConfigStore rejected(o);
        QTRY_COMPARE(rejected.status(), ConfigStore::Status::Error);
        QCOMPARE(rejected.value({"strict"}).toInt(), 4);
        QVERIFY(!rejected.error().contains("synthetic"));
        auto shipped = options();
        shipped.layers[0].shippedDefaults = true;
        shipped.layers[0].writable = false;
        shipped.layers[0].sources[0].raw = {{"n", QString("synthetic-invalid")}};
        ConfigStore invalidShipped(shipped);
        QTRY_COMPARE(invalidShipped.status(), ConfigStore::Status::Error);
    }
    void freshDiskFenceWithoutReload() {
        QTemporaryDir dir;
        writeFile(dir.filePath("a.json"), "{\"n\":1}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"a", "n"}});
        draft->setValue({"a", "n"}, 7);
        writeFile(dir.filePath("a.json"), "{\"n\":2}");
        const auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.error.code, QString("draft-conflict"));
        QVERIFY(draft->dirty() && draft->conflicted());
        QCOMPARE(store.value({"a", "n"}).toInt(), 2);
        QCOMPARE(readFile(dir.filePath("a.json")), QByteArray("{\"n\":2}"));
    }
    void maskedDraftAndBusyLifetime() {
        auto o = options();
        o.layers.append({"runtime",
                         ConfigLayerKind::Runtime,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"runtime", {}, {{"a", QVariantMap{{"n", 99}}}}}},
                         false});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"a", "n"}});
        QCOMPARE(draft->value({"a", "n"}).toInt(), 99);
        QVERIFY(draft->setValue({"a", "n"}, 7).accepted);
        QCOMPARE(draft->value({"a", "n"}).toInt(), 7);
        QCOMPARE(store.value({"a", "n"}).toInt(), 99);
        ConfigStoreTestAccess::run(store, [](auto &worker) {
            worker.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
        });
        bool cancelled = true;
        const auto id = draft->apply(this, [&](const auto &) { cancelled = false; });
        QVERIFY(draft->busy());
        QVERIFY(!draft->discard());
        QVERIFY(!draft->setValue({"a", "n"}, 8).accepted);
        delete draft;
        QSignalSpy results(&store, &ConfigStore::completed);
        ConfigStoreTestAccess::run(store, [](auto &worker) { worker.releaseBackend(); });
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(results.first().first().value<ConfigResult>().requestId, id);
        QCOMPARE(results.first().first().value<ConfigResult>().status, QString("saved"));
        QCOMPARE(results.first().first().value<ConfigResult>().masks.value({"a", "n"}),
                 QString("runtime"));
        QVERIFY(cancelled);
        QCOMPARE(store.value({"a", "n"}).toInt(), 99);
        store.clearRuntime({"a", "n"});
        QTRY_COMPARE(store.value({"a", "n"}).toInt(), 7);
    }
    void profileResetMaliciousInputAndPartialReceipt() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto profile = dir.filePath("empty.json");
        request(store, [&] { return store.exportProfile("Empty", profile, {{"a", "n"}}); });
        store.setUser({"a", "n"}, 7);
        store.setUser({"b", "n"}, 8);
        request(store, [&] { return store.save(); });
        QCOMPARE(request(store, [&] { return store.importProfile(profile); }).status,
                 QString("saved"));
        QCOMPARE(store.value({"a", "n"}).toInt(), 0);
        QCOMPARE(store.value({"b", "n"}).toInt(), 8);
        const auto original = readFile(dir.filePath("a.json"));
        QCOMPARE(request(store, [&] { return store.exportProfile("Bad", dir.filePath("a.json")); })
                     .error.code,
                 QString("profile-is-config-source"));
        QCOMPARE(readFile(dir.filePath("a.json")), original);
        writeFile(profile,
                  "{\"format\":1,\"name\":\"Injected\",\"entries\":[{\"path\":[\"a\",\"secret\"],"
                  "\"present\":true,\"value\":\"synthetic-secret\"}]}");
        QCOMPARE(request(store, [&] { return store.importProfile(profile); }).status,
                 QString("failed"));
        QCOMPARE(store.value({"a", "secret"}).toString(), QString("private-default"));
        QCOMPARE(readFile(dir.filePath("a.json")), original);
        request(store, [&] { return store.exportProfile("Both", profile); });
        auto destination = options(dir.path() + "/destination");
        destination.fileFault = [](const QString &phase, const QString &file) {
            return phase == "write" && file.endsWith("b.json") ? QString("synthetic-fault")
                                                               : QString();
        };
        ConfigStore target(destination);
        QTRY_COMPARE(target.status(), ConfigStore::Status::Ready);
        const auto result = request(target, [&] { return target.importProfile(profile); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("partial"));
        QCOMPARE(target.value({"b", "n"}).toInt(), 8);
    }
    void strictBatchRetainsWholeLayerAndRaw() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        auto root = *o.schema.descriptor({});
        root.fields["b"].fields["n"].admissionPolicy = D::AdmissionPolicy::Strict;
        o.schema = ConfigSchema(root);
        writeFile(dir.filePath("a.json"), "{\"n\":1}");
        writeFile(dir.filePath("b.json"), "{\"n\":2}");
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        writeFile(dir.filePath("a.json"), "{\"n\":3}");
        writeFile(dir.filePath("b.json"), "{\"n\":\"synthetic-invalid\"}");
        QCOMPARE(request(store, [&] { return store.reload(); }).status, QString("failed"));
        QCOMPARE(store.value({"a", "n"}).toInt(), 1);
        QCOMPARE(store.value({"b", "n"}).toInt(), 2);
        QVERIFY(!store.info({"b", "n"}).provenance.contributions.last().valid);
        const auto exported = dir.filePath("raw.json");
        request(store, [&] { return store.exportProfile("Raw", exported, {{"a", "n"}}); });
        QVERIFY(readFile(exported).contains("3")); // New raw retained apart from old projection.
    }
    void migrationCompletionAfterPersistenceAndReload() {
        QTemporaryDir dir;
        const QByteArray original = "{\"old\":7,\"unknown\":true}";
        writeFile(dir.filePath("a.json"), original);
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1, {{0, [](const QVariantMap &input) {
                                                            auto raw = input;
                                                            if (raw.contains("old"))
                                                                raw["n"] = raw.take("old");
                                                            return ConfigValueResult{raw, {}};
                                                        }}});
        auto entered = std::make_shared<std::atomic_bool>(false);
        auto release = std::make_shared<QSemaphore>();
        o.fileFault = [entered, release](const QString &phase, const QString &) {
            if (phase == "write") {
                *entered = true;
                release->acquire();
            }
            return QString();
        };
        ConfigStore store(o);
        QSignalSpy results(&store, &ConfigStore::migrationCompleted);
        QTRY_VERIFY(entered->load());
        QCOMPARE(store.status(), ConfigStore::Status::Loading);
        QCOMPARE(results.size(), 0);
        QCOMPARE(readFile(dir.filePath("a.json")), original);
        release->release(2); // Startup and later reload each publish once.
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(results.size(), 1);
        // A foreign old-version b shard must go through the same migration on reload.
        writeFile(dir.filePath("b.json"), "{\"old\":9}");
        QCOMPARE(request(store, [&] { return store.reload(); }).status, QString("reloaded"));
        QCOMPARE(store.value({"b", "n"}).toInt(), 9);
        QCOMPARE(readFile(dir.filePath("b.json.configqml-v0.bak")), QByteArray("{\"old\":9}"));
    }
    void secretMetadataAndInputEvidence() {
        auto o = options();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"a", "secret"}, QString("synthetic-secret"));
        store.setUser({"a", "reference"}, QString("keyring:synthetic"));
        store.setUser({"a", "n"}, 7);
        request(store, [&] { return store.save(); });
        const auto inputs = store.inputs();
        QVERIFY(!inputs[0].value.isValid());
        QVERIFY(!inputs[1].value.isValid());
        QCOMPARE(inputs[2].value.toInt(), 7);
        QCOMPARE(store.value({"a", "secret"}).toString(), QString("synthetic-secret"));
        auto root = *o.schema.descriptor({});
        root.fields["a"].sensitive = true;
        ConfigSchema inherited(root);
        QVERIFY(inherited.sensitive({"a", "n"}));
        QVERIFY(!inherited.portable({"a", "n"}));
        auto field = D::leaf(T::String, QString("synthetic-default"));
        field.sensitive = true;
        auto record = D::leaf(T::Record, QVariantMap{{"private", QString("synthetic-default")}});
        record.fields = {{"private", field}};
        ConfigSchema atomic(D::group({{"record", record}}));
        QVERIFY(!atomic.portable({"record"}));
        QVERIFY(atomic.sensitive({"record"}));
    }
    void migrationPartialAndBackupEvidence() {
        QTemporaryDir dir;
        const QByteArray original = "{\"old\":7,\"unknown\":true}";
        writeFile(dir.filePath("a.json"), original);
        writeFile(dir.filePath("b.json"), original);
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1, {{0, [](const QVariantMap &input) {
                                                            auto raw = input;
                                                            raw["n"] = raw.take("old");
                                                            return ConfigValueResult{raw, {}};
                                                        }}});
        o.fileFault = [](const QString &phase, const QString &file) {
            return phase == "write" && file.endsWith("b.json") ? QString("synthetic-fault")
                                                               : QString();
        };
        ConfigStore store(o);
        QSignalSpy results(&store, &ConfigStore::migrationCompleted);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QCOMPARE(results.size(), 1);
        const auto result = results.first().first().value<ConfigResult>();
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("partial"));
        QCOMPARE(result.publications.size(), 2);
        QCOMPARE(readFile(dir.filePath("b.json")), original);
        QCOMPARE(readFile(dir.filePath("a.json.configqml-v0.bak")), original);
        QCOMPARE(readFile(dir.filePath("b.json.configqml-v0.bak")), original);
        QVERIFY(readFile(dir.filePath("a.json")).contains("_configqmlVersion"));
    }
    void thousandRowDraftBudget() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"a", "rows"}});
        QVariantList rows;
        for (int i = 0; i < 1000; ++i)
            rows.append(i);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(draft->setValue({"a", "rows"}, rows).accepted);
        const auto id = draft->apply();
        const auto ingress = timer.nsecsElapsed();
        QSignalSpy results(&store, &ConfigStore::completed);
        QTRY_VERIFY(!results.isEmpty());
        QCOMPARE(results.last().first().value<ConfigResult>().requestId, id);
        QCOMPARE(store.listModel({"a", "rows"})->rowCount(), 1000);
        QCOMPARE(store.value({"a", "rows"}).toList().last().toInt(), 999);
        const auto publication = ConfigStoreTestAccess::publicationLibraryNanos(store);
        qInfo("draft 1000 native ingress=%lldus publication=%lldus", ingress / 1000,
              publication / 1000);
        if (enforceModelBudgets)
            QVERIFY(ingress + publication < 16000000);
    }
};
QTEST_GUILESS_MAIN(EditorsTest)
#include "test_editors.moc"
