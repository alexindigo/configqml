// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "fileoptions.h"
#include <QTemporaryDir>
#include <QDir>
#include <sys/stat.h>
#include <unistd.h>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
ConfigStoreOptions options(const QString &directory, const QString &path = {}) {
    auto o = fileOptions(path.isEmpty() ? directory + "/user.json" : path, ConfigWatchMode::Live);
    o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictSourcePreimage;
    o.jsonFiles[0].deleteWhenEmpty = true;
    o.evidenceDirectory = directory + "/evidence";
    auto root = *o.schema.descriptor({});
    root.fields["record"] = D::leaf(T::Record, QVariantMap{});
    root.fields["flag"] = D::leaf(T::Boolean, true);
    o.schema = ConfigSchema(root);
    o.layers.prepend({"legacy",
                      ConfigLayerKind::File,
                      ConfigAdmissionPolicy::FieldFallback,
                      {{"old", {}, {{"a", 7}}}},
                      false});
    return o;
}
} // namespace
class EmptyShardsTest : public QObject {
    Q_OBJECT
private slots:
    void readOnlyDeclaration() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.layers.last().writable = false;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QCOMPARE(store.error(), QString("empty-delete-readonly"));
    }
    void deleteAndRestart() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":3}");
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"a"}, "user").accepted);
        QTRY_COMPARE(store.value({"a"}).toInt(), 7);
        const auto result = compat::request(store, [&] { return store.save(); });
        QCOMPARE(result.status, QString("saved"));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(path + ".lock"));
        QVERIFY(result.publications.last().success);
        QVERIFY(!result.publications.last().revision.exists);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 7);
        QCOMPARE(compat::request(restart, [&] { return restart.save(); }).status,
                 QString("nothing_to_save"));
        QVERIFY(!QFileInfo::exists(path));
    }
    void unknownAndAtomicChoices_data() {
        QTest::addColumn<QByteArray>("choice");
        QTest::newRow("unknown-empty-object") << QByteArray("\"opaque\":{}");
        QTest::newRow("unknown-zero") << QByteArray("\"opaque\":0");
        QTest::newRow("atomic-empty-list") << QByteArray("\"items\":[]");
        QTest::newRow("atomic-null") << QByteArray("\"nullable\":null");
        QTest::newRow("atomic-zero") << QByteArray("\"b\":0");
        QTest::newRow("atomic-empty-record") << QByteArray("\"record\":{}");
        QTest::newRow("atomic-false") << QByteArray("\"flag\":false");
    }
    void unknownAndAtomicChoices() {
        QFETCH(QByteArray, choice);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":3," + choice + "}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"a"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(compat::bytes(path).contains(choice));
    }
    void versionOnlyAndDeclaredScaffolding() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"_configqmlVersion\":0,\"section\":{\"a\":3}}");
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(D::group({{"section", D::group({{"a", D::leaf(T::SafeInteger, 0)}})}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"section", "a"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(!QFileInfo::exists(path));
    }
    void symlinkDeleteRecreateAndWatch() {
        QTemporaryDir dir;
        const auto target = dir.filePath("target.json"), link = dir.filePath("link.json");
        compat::put(target, "{\"a\":3}");
        QVERIFY(QFile::link(target, link));
        struct stat before, after;
        QVERIFY(::lstat(QFile::encodeName(link).constData(), &before) == 0);
        ConfigStore store(options(dir.path(), link));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"a"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(::lstat(QFile::encodeName(link).constData(), &after) == 0);
        QCOMPARE(after.st_ino, before.st_ino);
        QCOMPARE(QFileInfo(link).symLinkTarget(), target);
        QVERIFY(QFileInfo::exists(target + ".lock"));
        QVERIFY(store.setUser({"a"}, 5).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(::lstat(QFile::encodeName(link).constData(), &after) == 0);
        QCOMPARE(after.st_ino, before.st_ino);
        compat::put(target, "{\"a\":8}");
        QTRY_COMPARE(store.value({"a"}).toInt(), 8);
        QVERIFY(QFile::remove(target));
        QTRY_COMPARE(store.value({"a"}).toInt(), 7);
        compat::put(target, "{\"a\":9}");
        QTRY_COMPARE(store.value({"a"}).toInt(), 9);
    }
    void cleanupCannotDeleteNonobjectAncestor() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"section\":false}");
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(D::group({{"section", D::group({{"a", D::leaf(T::SafeInteger, 0)}})}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"section", "a"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(compat::bytes(path).contains("\"section\":false"));
        QVERIFY(store.reset({"section"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(!QFileInfo::exists(path));
    }
    void parentLinkRetargetRace() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("one")));
        QVERIFY(QDir().mkpath(dir.filePath("two")));
        compat::put(dir.filePath("one/user.json"), "{\"a\":3}");
        compat::put(dir.filePath("two/user.json"), "{\"a\":99}");
        const auto link = dir.filePath("parent");
        QVERIFY(QFile::link(dir.filePath("one"), link));
        auto o = options(dir.path(), link + "/user.json");
        o.fileFault = [link, destination = dir.filePath("two")](const QString &phase,
                                                                const QString &) {
            if (phase == "delete") {
                if (!QFile::remove(link) || !QFile::link(destination, link))
                    qFatal("retarget fixture");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"a"}, "user").accepted);
        const auto result = compat::request(store, [&] { return store.save(); });
        QCOMPARE(result.error.code, QString("empty-delete-target"));
        QCOMPARE(compat::bytes(dir.filePath("one/user.json")), QByteArray("{\"a\":3}"));
        QCOMPARE(compat::bytes(dir.filePath("two/user.json")), QByteArray("{\"a\":99}"));
    }
    void hardLinkedTargetIsNotDeleted() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":3}");
        QVERIFY(::link(QFile::encodeName(path).constData(),
                       QFile::encodeName(dir.filePath("other.json")).constData()) == 0);
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"a"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("hard-linked-target"));
        QCOMPARE(compat::bytes(path), QByteArray("{\"a\":3}"));
    }
    void deleteFault_data() {
        QTest::addColumn<QString>("phase");
        QTest::newRow("fsync") << QString("directory-sync");
        QTest::newRow("readback") << QString("readback");
    }
    void deleteFault() {
        QFETCH(QString, phase);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":3}");
        auto o = options(dir.path());
        o.fileFault = [phase](const QString &p, const QString &) {
            return p == phase ? QString("delete-fault") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"a"}, "user").accepted);
        const auto result = compat::request(store, [&] { return store.save(); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("uncertain"));
        QCOMPARE(result.error.code, QString("delete-fault"));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(result.coveredRevisions.isEmpty());
        bool absentEvidence = false;
        for (const auto &ref : result.evidence)
            if (ref.role == "published") {
                const auto meta =
                    decodeJsonValue(compat::bytes(ref.directory + "/metadata.json")).value.toMap();
                QCOMPARE(meta.value("presence").toString(), QString("absent"));
                QCOMPARE(meta.value("bytesState").toString(), QString("absent"));
                QVERIFY(!QFileInfo::exists(ref.directory + "/bytes"));
                absentEvidence = true;
            }
        QVERIFY(absentEvidence);
    }
};
QTEST_GUILESS_MAIN(EmptyShardsTest)
#include "test_empty_shards.moc"
