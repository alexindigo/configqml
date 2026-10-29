// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include <QTemporaryDir>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class StartupTest : public QObject {
    Q_OBJECT
private slots:
    void skipQuarantineRepairAndFailReady() {
        QTemporaryDir dir;
        compat::put(dir.filePath("bad.json"), "malformed bytes");
        compat::put(dir.filePath("good.json"), "{\"n\":8}");
        ConfigStoreOptions o;
        o.autoSave = false;
        o.evidenceDirectory = dir.filePath("evidence");
        o.schema =
            ConfigSchema(D::group({{"bad", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                   {"good", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"bad", {"bad"}, {}}, {"good", {"good"}, {}}},
                     true}};
        o.jsonFiles = {{"user", "bad", dir.filePath("bad.json"), true,
                        ConfigWatchMode::StartupSnapshot,
                        ConfigStartupErrorPolicy::SkipProjectionRetainError},
                       {"user", "good", dir.filePath("good.json")}};
        ConfigStore store(o);
        bool metadataBeforeReady = false;
        connect(&store, &ConfigStore::sourceStatesChanged, this,
                [&] { metadataBeforeReady = true; });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(metadataBeforeReady);
        QCOMPARE(store.value({"good", "n"}).toInt(), 8);
        const auto state = store.sourceStates().first();
        QCOMPARE(state.presence, QString("present"));
        QCOMPARE(state.load, QString("parse_error"));
        QVERIFY(state.quarantined);
        QVERIFY(!state.evidence.isEmpty());
        QCOMPARE(compat::bytes(state.evidence.first().directory + "/bytes"),
                 QByteArray("malformed bytes"));
        const auto rejected =
            compat::request(store, [&] { return store.setUser({"bad", "n"}, 3).requestId; });
        QCOMPARE(rejected.error.code, QString("source-quarantined"));
        QCOMPARE(compat::bytes(dir.filePath("bad.json")), QByteArray("malformed bytes"));
        compat::put(dir.filePath("bad.json"), "{\"n\":4}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QVERIFY(!store.sourceStates().first().quarantined);
        QCOMPARE(store.value({"bad", "n"}).toInt(), 4);
        compat::put(dir.filePath("bad.json"), "bad again");
        o.jsonFiles.first().startupErrorPolicy = ConfigStartupErrorPolicy::FailReady;
        ConfigStore strict(o);
        QTRY_COMPARE(strict.status(), ConfigStore::Status::Error);
    }
};
QTEST_GUILESS_MAIN(StartupTest)
#include "test_startup_policy.moc"
