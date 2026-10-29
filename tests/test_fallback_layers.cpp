// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include <QTemporaryDir>
#include <sys/stat.h>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
static ConfigStoreOptions options(const QString &directory) {
    D extra;
    extra.optional = true;
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group({{"settings", D::group({{"a", D::leaf(T::SafeInteger, 0)},
                                                             {"b", D::leaf(T::SafeInteger, 0)},
                                                             {"extra", extra}})}}));
    ConfigSource legacy{"old", {"settings"}, {}};
    legacy.fallbackFor = "new";
    o.layers = {{"legacy", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {legacy}},
                {"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"new", {"settings"}, {}}},
                 true}};
    o.jsonFiles = {{"legacy", "old", directory + "/old.json"},
                   {"user", "new", directory + "/new.json"}};
    return o;
}
class FallbackTest : public QObject {
    Q_OBJECT
private slots:
    void rawSelection_data() {
        QTest::addColumn<QByteArray>("raw");
        QTest::addColumn<int>("a");
        QTest::addColumn<int>("b");
        QTest::addColumn<bool>("extra");
        QTest::newRow("fallback-absent") << QByteArray() << 1 << 2 << true;
        QTest::newRow("fallback-empty") << QByteArray("{}") << 1 << 2 << true;
        QTest::newRow("fallback-nonempty-suppresses-whole-source")
            << QByteArray("{\"a\":3}") << 3 << 0 << false;
        QTest::newRow("fallback-invalid-present") << QByteArray("{\"a\":false}") << 0 << 0 << false;
        QTest::newRow("fallback-unknown-only-raw")
            << QByteArray("{\"opaque\":0}") << 0 << 0 << false;
        QTest::newRow("fallback-unknown-object")
            << QByteArray("{\"opaque\":{}}") << 0 << 0 << false;
        QTest::newRow("fallback-version-only")
            << QByteArray("{\"_configqmlVersion\":0}") << 0 << 0 << false;
    }
    void rawSelection() {
        QFETCH(QByteArray, raw);
        QFETCH(int, a);
        QFETCH(int, b);
        QFETCH(bool, extra);
        QTemporaryDir dir;
        const auto legacy = QByteArray("{\"a\":1,\"b\":2,\"extra\":\"legacy\"}");
        compat::put(dir.filePath("old.json"), legacy);
        QVERIFY(QFile::setPermissions(dir.filePath("old.json"), QFileDevice::ReadOwner));
        if (!raw.isEmpty())
            compat::put(dir.filePath("new.json"), raw);
        struct stat before, after;
        QVERIFY(::lstat(QFile::encodeName(dir.filePath("old.json")).constData(), &before) == 0);
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"settings", "a"}).toInt(), a);
        QCOMPARE(store.value({"settings", "b"}).toInt(), b);
        QCOMPARE(store.info({"settings", "extra"}).provenance.present, extra);
        QCOMPARE(store.sourceStates().first().fallbackState,
                 extra ? QString("active") : QString("suppressed"));
        if (raw.isEmpty())
            QVERIFY(!QFile::exists(dir.filePath("new.json")));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"settings", "a"}).toInt(), a);
        QCOMPARE(reopened.value({"settings", "b"}).toInt(), b);
        QCOMPARE(compat::bytes(dir.filePath("old.json")), legacy);
        QVERIFY(::lstat(QFile::encodeName(dir.filePath("old.json")).constData(), &after) == 0);
        QCOMPARE(before.st_ino, after.st_ino);
        QCOMPARE(before.st_mtim.tv_sec, after.st_mtim.tv_sec);
        QCOMPARE(before.st_mtim.tv_nsec, after.st_mtim.tv_nsec);
    }
    void resetAndForeignRepair() {
        QTemporaryDir dir;
        compat::put(dir.filePath("old.json"), "{\"a\":1,\"b\":2,\"extra\":\"legacy\"}");
        compat::put(dir.filePath("new.json"), "{\"a\":3,\"opaque\":0}");
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"settings", "a"}, "user").accepted);
        QTRY_COMPARE(store.info({"settings", "a"}).workerCovered, quint64(1));
        QCOMPARE(store.value({"settings", "b"}).toInt(), 0);
        QVERIFY(store.reset({"settings"}, "user").accepted);
        QTRY_COMPARE(store.value({"settings", "b"}).toInt(), 2);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"settings", "b"}).toInt(), 2);
        compat::put(dir.filePath("new.json"), "{\"a\":3}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(store.value({"settings", "b"}).toInt(), 0);
        compat::put(dir.filePath("new.json"), "bad");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status, QString("failed"));
        QCOMPARE(store.value({"settings", "a"}).toInt(), 3);
        QCOMPARE(store.value({"settings", "b"}).toInt(), 0);
        compat::put(dir.filePath("new.json"), "{}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(store.value({"settings", "b"}).toInt(), 2);
    }
    void invalidTopology() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.layers.first().writable = true;
        ConfigLayers writable(o.schema, o.layers);
        QVERIFY(!writable.valid());
        o = options(dir.path());
        o.layers.first().sources.first().fallbackFor = "unknown";
        ConfigLayers unknown(o.schema, o.layers);
        QVERIFY(!unknown.valid());
        o = options(dir.path());
        o.layers.last().sources.first().fallbackFor = "old";
        ConfigLayers chained(o.schema, o.layers);
        QVERIFY(!chained.valid());
    }
};
QTEST_GUILESS_MAIN(FallbackTest)
#include "test_fallback_layers.moc"
