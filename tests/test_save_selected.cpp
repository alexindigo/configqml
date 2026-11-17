// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "storeprobe.h"
#include "confignumber.h"
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QSemaphore>
#include <atomic>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
ConfigStoreOptions options(const QString &directory, bool shards = false) {
    ConfigStoreOptions o;
    o.autoSave = false;
    o.evidenceDirectory = directory + "/evidence";
    o.schema = ConfigSchema(D::group(
        {{"ui", D::group({{"a", D::leaf(T::SafeInteger, 0)}, {"b", D::leaf(T::SafeInteger, 0)}})},
         {"binding", D::group({{"key", D::leaf(T::SafeInteger, 0)}})},
         {"other", D::leaf(T::SafeInteger, 0)}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"user", {}, {}}},
                 true}};
    if (shards) {
        o.layers[0].sources = {{"ui", {"ui"}, {}}, {"binding", {"binding"}, {}}};
        o.jsonFiles = {{"user", "ui", directory + "/ui.json"},
                       {"user", "binding", directory + "/binding.json"}};
    } else
        o.jsonFiles = {{"user", "user", directory + "/user.json"}};
    return o;
}
QVariantMap raw(const QString &path) {
    if (!QFileInfo::exists(path))
        return {};
    const auto decoded = ConfigJsonCodec().decode(compat::bytes(path), "user");
    if (!decoded.ok())
        qFatal("invalid selected fixture JSON");
    return decoded.raw;
}
int number(const QVariantMap &data, const ConfigPath &path) {
    const auto value = valueAt(data, path);
    return value ? D::leaf(T::SafeInteger, 0).validate(*value).value.toInt() : 0;
}
void control(ConfigStore &store, const std::function<void(ConfigWorker &)> &action) {
    QEventLoop loop;
    bool finished = false;
    ConfigStoreTestAccess::run(store, action, &loop, [&] {
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!finished)
        qFatal("selected worker control timeout");
}
} // namespace
class SaveSelectedTest : public QObject {
    Q_OBJECT
private slots:
    void selection_data() {
        QTest::addColumn<QList<ConfigPath>>("paths");
        QTest::addColumn<bool>("b");
        QTest::addColumn<bool>("key");
        QTest::newRow("leaf") << QList<ConfigPath>{{"ui", "a"}} << false << false;
        QTest::newRow("namespace") << QList<ConfigPath>{{"ui"}} << true << false;
        QTest::newRow("overlapping-union")
            << QList<ConfigPath>{{"ui"}, {"ui", "a"}} << true << false;
        QTest::newRow("disjoint") << QList<ConfigPath>{{"ui", "a"}, {"binding", "key"}} << false
                                  << true;
        QTest::newRow("root-namespace") << QList<ConfigPath>{ConfigPath{}} << true << true;
    }
    void selection() {
        QFETCH(QList<ConfigPath>, paths);
        QFETCH(bool, b);
        QFETCH(bool, key);
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"opaque\":18446744073709551615,\"ui\":{\"unknown\":true}}");
        const auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"ui", "a"}, 1).accepted);
        QVERIFY(store.setUser({"ui", "b"}, 2).accepted);
        QVERIFY(store.setUser({"binding", "key"}, 3).accepted);
        const auto inputs = store.inputs().size();
        const auto saved = compat::request(store, [&] { return store.saveSelected(paths); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(saved.operation, QString("saveSelected"));
        QCOMPARE(saved.inputPrefix, quint64(inputs));
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(saved.coverage.size(), 1 + int(b) + int(key));
        auto data = raw(file);
        QCOMPARE(number(data, {"ui", "a"}), 1);
        QCOMPARE(number(data, {"ui", "b"}), b ? 2 : 0);
        QCOMPARE(number(data, {"binding", "key"}), key ? 3 : 0);
        QCOMPARE(valueAt(data, {"ui", "unknown"})->toBool(), true);
        QCOMPARE(data["opaque"].value<ConfigNumber>().token(), QString("18446744073709551615"));
        ConfigStore cold(o);
        QTRY_COMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(cold.value({"ui", "a"}).toInt(), 1);
        QCOMPARE(cold.value({"ui", "b"}).toInt(), b ? 2 : 0);
        QCOMPARE(cold.value({"binding", "key"}).toInt(), key ? 3 : 0);
        const auto noOp = compat::request(store, [&] { return store.saveSelected(paths); });
        QCOMPARE(noOp.status, QString("nothing_to_save"));
        QVERIFY(noOp.coverage.isEmpty());
        QVERIFY(noOp.publications.isEmpty());
        QVERIFY(noOp.coveredRevisions.isEmpty());
        QVERIFY(compat::request(store, [&] { return store.save(); }).status != "failed");
        QCOMPARE(number(raw(file), {"ui", "b"}), 2);
        QCOMPARE(number(raw(file), {"binding", "key"}), 3);
    }
    void reservations_data() {
        QTest::addColumn<bool>("shards");
        QTest::newRow("same-source") << false;
        QTest::newRow("different-source") << true;
    }
    void reservations() {
        QFETCH(bool, shards);
        QTemporaryDir dir;
        auto o = options(dir.path(), shards);
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const ConfigPath key{"binding", "key"};
        const auto retained = compat::request(store, [&] {
            return store.retainGroup({"head", {{"binding"}}, {{key, true, 7}}, {}});
        });
        QVERIFY(!retained.groupToken.isEmpty());
        // Make a real stopped attempt without changing the retained command.
        const auto lock = dir.filePath(shards ? "binding.json.lock" : "user.json.lock");
        QVERIFY(QDir().mkdir(lock));
        const auto failed =
            compat::request(store, [&] { return store.saveGroup(retained.groupToken); });
        QCOMPARE(failed.status, QString("failed"));
        QVERIFY(QDir().rmdir(lock));
        const auto original = store.groupInfo(retained.groupToken);
        QVERIFY(store.setUser({"ui", "a"}, 2).accepted);
        QVERIFY(store.setUser({"ui", "b"}, 9).accepted);
        const auto saved =
            compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); });
        QCOMPARE(saved.status, QString("saved"));
        QVERIFY(saved.unresolvedGroups.isEmpty());
        QCOMPARE(saved.excludedGroups, QStringList{retained.groupToken});
        const auto file = dir.filePath(shards ? "ui.json" : "user.json");
        QCOMPARE(number(raw(file), shards ? ConfigPath{"a"} : ConfigPath{"ui", "a"}), 2);
        QCOMPARE(number(raw(file), shards ? ConfigPath{"b"} : ConfigPath{"ui", "b"}), 0);
        const auto before = compat::bytes(file);
        const auto refused = compat::request(
            store, [&] { return store.saveSelected({{"ui"}, {"binding", "key"}}); });
        QCOMPARE(refused.error.code, QString("unresolved-group"));
        QCOMPARE(refused.unresolvedGroups, QStringList{retained.groupToken});
        QVERIFY(refused.publications.isEmpty());
        QCOMPARE(compat::bytes(file), before);
        const auto current = store.groupInfo(retained.groupToken);
        QCOMPARE(current.state, original.state);
        QCOMPARE(current.capturePrefix, original.capturePrefix);
        QCOMPARE(current.attempts.size(), original.attempts.size());
        QCOMPARE(current.attempts.first().attemptId, failed.attemptId);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("unresolved-group"));
        const auto retry =
            compat::request(store, [&] { return store.retryGroup(retained.groupToken); });
        QCOMPARE(retry.status, QString("saved"));
        QCOMPARE(retry.capturePrefix, retained.capturePrefix);
        QCOMPARE(retry.attemptNumber, quint64(2));
        QCOMPARE(retry.requestKey, retained.requestKey);
        QCOMPARE(number(raw(dir.filePath(shards ? "binding.json" : "user.json")),
                        shards ? ConfigPath{"key"} : key),
                 7);
    }
    void invalidSelectionsDoNotPublishOrPoison() {
        QTemporaryDir dir;
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"ui", "a"}, 4).accepted);
        const QList<QList<ConfigPath>> invalid{{},
                                               {{"missing"}},
                                               {{"ui", "a", "member"}},
                                               {{"ui", "a"}, {"missing"}},
                                               {{"ui"}, {"ui"}}};
        for (const auto &paths : invalid) {
            const auto failed = compat::request(store, [&] { return store.saveSelected(paths); });
            QCOMPARE(failed.status, QString("failed"));
            QCOMPARE(failed.error.code, QString("save-selection"));
            QVERIFY(failed.publications.isEmpty());
            QVERIFY(!QFileInfo::exists(dir.filePath("user.json")));
        }
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui"}}); }).status,
                 QString("saved"));
        auto readOnly = options(dir.path());
        readOnly.layers[0].writable = false;
        ConfigStore reader(readOnly);
        QTRY_COMPARE(reader.status(), ConfigStore::Status::Ready);
        QCOMPARE(compat::request(reader, [&] { return reader.saveSelected({{"ui"}}); }).error.code,
                 QString("save-selection"));
    }
    void namespaceResetFragmentsAndUnknowns() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"ui\":{\"a\":1,\"b\":2,\"unknown\":3},\"opaque\":1e400}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"ui"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("saved"));
        auto data = raw(file);
        QVERIFY(!valueAt(data, {"ui", "a"}));
        QCOMPARE(number(data, {"ui", "b"}), 2);
        QCOMPARE(number(data, {"ui", "unknown"}), 3);
        QCOMPARE(data["opaque"].value<ConfigNumber>().token(), QString("1e400"));
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui"}}); }).status,
                 QString("saved"));
        data = raw(file);
        QVERIFY(!data.contains("ui"));
        QVERIFY(data.contains("opaque"));
    }
    void selectedResetKeepsReservedCleanup_data() {
        QTest::addColumn<bool>("strict");
        QTest::newRow("path-intent") << false;
        QTest::newRow("strict-pending-rebase") << true;
    }
    void selectedResetKeepsReservedCleanup() {
        QFETCH(bool, strict);
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"ui\":{\"a\":1,\"b\":2,\"unknown\":3},\"opaque\":1e400}");
        auto o = options(dir.path());
        if (strict)
            o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"ui"}, "user").accepted);
        const auto retained = compat::request(
            store, [&] { return store.retainGroup({"reset-b", {{"ui", "b"}}, {}, {}}); });
        QCOMPARE(retained.status, QString("retained"));
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("saved"));
        const auto data = raw(file);
        QVERIFY(!valueAt(data, {"ui", "a"}));
        QCOMPARE(number(data, {"ui", "b"}), 2);
        QCOMPARE(number(data, {"ui", "unknown"}), 3);
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(
            compat::request(store, [&] { return store.saveGroup(retained.groupToken); }).status,
            QString("saved"));
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui"}}); }).status,
                 QString("saved"));
        QVERIFY(!raw(file).contains("ui"));
        QVERIFY(raw(file).contains("opaque"));
    }
    void namespaceCleanupSettlesAfterNewerSelectedValue() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"ui\":{\"a\":1,\"b\":2,\"unknown\":3}}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"ui"}, "user").accepted);
        QVERIFY(store.setUser({"ui", "a"}, 9).accepted);
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui"}}); }).status,
                 QString("saved"));
        QCOMPARE(number(raw(file), {"ui", "a"}), 9);
        QVERIFY(!valueAt(raw(file), {"ui", "b"}));
        QVERIFY(!valueAt(raw(file), {"ui", "unknown"}));
        const auto noOp = compat::request(store, [&] { return store.saveSelected({{"ui"}}); });
        QCOMPARE(noOp.status, QString("nothing_to_save"));
        QVERIFY(noOp.coverage.isEmpty());
        QVERIFY(noOp.coveredRevisions.isEmpty());
    }
    void orderedPrefixAndOwnedPaths() {
        QTemporaryDir dir;
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        control(store, [](auto &w) { w.holdInputs(true); });
        QVERIFY(store.setUser({"ui", "a"}, 1).accepted);
        QChar storage[]{QChar('a')};
        ConfigPath path{QString("ui"), QString::fromRawData(storage, 1)};
        QList<ConfigResult> receipts;
        store.saveSelected({path}, this, [&](const auto &r) { receipts.append(r); });
        storage[0] = QChar('b');
        QVERIFY(store.setUser({"ui", "a"}, 2).accepted);
        control(store, [](auto &w) { w.releaseInputs(2); });
        QTRY_COMPARE(receipts.size(), 1);
        QCOMPARE(receipts[0].status, QString("saved"));
        QCOMPARE(receipts[0].inputPrefix, quint64(1));
        QCOMPARE(number(raw(dir.filePath("user.json")), {"ui", "a"}), 1);
        control(store, [](auto &w) { w.holdInputs(false); });
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("saved"));
        QCOMPARE(number(raw(dir.filePath("user.json")), {"ui", "a"}), 2);
    }
    void partialAndUncertainFailuresKeepOtherWork() {
        QTemporaryDir dir;
        auto o = options(dir.path(), true);
        bool fault = true;
        o.fileFault = [&](const QString &phase, const QString &file) {
            return fault && file.endsWith("/ui.json") && phase == "readback"
                       ? QString("selected-readback")
                       : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"ui", "a"}, 3).accepted);
        QVERIFY(store.setUser({"ui", "b"}, 8).accepted);
        QVERIFY(store.setUser({"binding", "key"}, 4).accepted);
        const auto failed =
            compat::request(store, [&] { return store.saveSelected({{"ui", "a"}, {"binding"}}); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, QString("selected-readback"));
        QCOMPARE(failed.publicationState, QString("partial"));
        QVERIFY(failed.coveredRevisions.contains("binding"));
        QVERIFY(!failed.coveredRevisions.contains("ui"));
        QVERIFY(!failed.evidence.isEmpty());
        QCOMPARE(number(raw(dir.filePath("ui.json")), {"a"}), 3);
        QCOMPARE(number(raw(dir.filePath("ui.json")), {"b"}), 0);
        fault = false;
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("saved"));
        QCOMPARE(number(raw(dir.filePath("ui.json")), {"b"}), 0);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(number(raw(dir.filePath("ui.json")), {"b"}), 8);
    }
    void selectedWorkKeepsUnselectedFrozenSourceFence() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        const QByteArray original = "{\"ui\":{\"a\":1,\"b\":2}}";
        const QByteArray foreign = "{\"ui\":{\"a\":3,\"b\":4}}";
        compat::put(file, original);
        auto o = options(dir.path());
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        auto fault = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [fault](const QString &phase, const QString &) {
            return phase == "write" && fault->exchange(false) ? QString("freeze-write") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"ui", "a"}, 9).accepted);
        compat::put(file, foreign);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("freeze-write"));
        QVERIFY(store.setUser({"ui", "b"}, 8).accepted);
        const auto refused =
            compat::request(store, [&] { return store.saveSelected({{"ui", "b"}}); });
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("source-preimage-conflict"));
        QCOMPARE(compat::bytes(file), foreign);
        compat::put(file, original);
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "b"}}); }).status,
                 QString("saved"));
        QCOMPARE(number(raw(file), {"ui", "a"}), 1);
        QCOMPARE(number(raw(file), {"ui", "b"}), 8);
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("saved"));
        QCOMPARE(number(raw(file), {"ui", "a"}), 9);
        QCOMPARE(number(raw(file), {"ui", "b"}), 8);
    }
    void remainingWorkKeepsOrdinaryAutosave() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.autoSave = true;
        o.autoSaveInterval = 200;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy autosaved(&store, &ConfigStore::autoSaved);
        QVERIFY(store.setUser({"ui", "a"}, 1).accepted);
        QVERIFY(store.setUser({"ui", "b"}, 2).accepted);
        QCOMPARE(compat::request(store, [&] { return store.saveSelected({{"ui", "a"}}); }).status,
                 QString("saved"));
        QCOMPARE(number(raw(dir.filePath("user.json")), {"ui", "b"}), 0);
        QTRY_VERIFY(!autosaved.isEmpty());
        QCOMPARE(number(raw(dir.filePath("user.json")), {"ui", "b"}), 2);
        QCOMPARE(qvariant_cast<ConfigResult>(autosaved.last()[0]).operation, QString("autosave"));
    }
    void destructionSettlesOnlyTheSelectedRequest() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        auto entered = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        auto writes = std::make_shared<std::atomic<int>>(0);
        o.fileFault = [entered, release, writes](const QString &phase, const QString &) {
            if (phase == "write" && ++*writes == 1) {
                entered->release();
                release->acquire();
            }
            return QString();
        };
        auto store = std::make_unique<ConfigStore>(o);
        QTRY_COMPARE(store->status(), ConfigStore::Status::Ready);
        QPointer<ConfigWorker> worker;
        control(*store, [&](auto &w) { worker = &w; });
        QVERIFY(store->setUser({"ui", "a"}, 1).accepted);
        QVERIFY(store->setUser({"ui", "b"}, 2).accepted);
        bool callback = false;
        store->saveSelected({{"ui", "a"}}, this, [&](const auto &) { callback = true; });
        const auto waiting = entered->tryAcquire(1, 10000);
        store.reset();
        release->release();
        QVERIFY(waiting);
        QTRY_VERIFY(worker.isNull());
        QCOMPARE(writes->load(), 1);
        QVERIFY(!callback);
        ConfigStore cold(o);
        QTRY_COMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(cold.value({"ui", "a"}).toInt(), 1);
        QCOMPARE(cold.value({"ui", "b"}).toInt(), 0);
    }
};
QTEST_GUILESS_MAIN(SaveSelectedTest)
#include "test_save_selected.moc"
