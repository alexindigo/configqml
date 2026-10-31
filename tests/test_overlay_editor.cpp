// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configoverlayeditor.h"
#include "configstorehandle.h"
#include "compatfixture.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QSaveFile>
#include <QProcess>
#include <limits>
#include <stdexcept>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
ConfigStoreOptions baseOptions(const QString &file = {}) {
    auto optional = D::leaf(T::String, QString());
    optional.defaultValue.reset();
    optional.optional = true;
    auto object = D::leaf(T::Record, QVariantMap{{"a", 1}, {"b", 2}});
    object.fields = {{"a", D::leaf(T::SafeInteger, 0)}, {"b", D::leaf(T::SafeInteger, 0)}};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(
        D::group({{"settings", D::group({{"volume", D::leaf(T::SafeInteger, 1)},
                                         {"object", object},
                                         {"optional", optional},
                                         {"list", D::list(D::leaf(T::SafeInteger, 0), {1, 2})}})},
                  {"extra", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"base", {"settings"}, {}}, {"extra", {"extra"}, {}}},
                 true}};
    if (!file.isEmpty())
        o.jsonFiles = {{"user", "base", file}};
    return o;
}
ConfigStoreOptions mainOptions(const QString &file = {}) {
    auto row = D::leaf(T::Record, QVariantMap{{"id", QString()}});
    row.fields = {{"id", D::leaf(T::String, QString())}};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema =
        ConfigSchema(D::group({{"rows", D::list(row, {})}, {"other", D::leaf(T::SafeInteger, 0)}}));
    o.layers = {
        {"user",
         ConfigLayerKind::File,
         ConfigAdmissionPolicy::FieldFallback,
         {{"main",
           {},
           {{"rows",
             QVariantList{QVariantMap{
                 {"id", "x"},
                 {"instanceSettings",
                  QVariantMap{{"volume", 2}, {"opaque", QVariantMap{{"keep", false}}}}}}}}}}},
         true}};
    if (!file.isEmpty())
        o.jsonFiles = {{"user", "main", file}};
    return o;
}
ConfigOverlayOptions selection(ConfigStore &main, ConfigStoreHandle *base,
                               ConfigOverlayMode mode = ConfigOverlayMode::Editor) {
    ConfigOverlayOptions o;
    o.mode = mode;
    o.base = base;
    o.ownerPath = {"rows"};
    o.ownerGeneration = main.info(o.ownerPath).publicationGeneration;
    o.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
    o.identityPath = {{ConfigValueSelector::Kind::Key, "id"}};
    o.identityValue = QString("x");
    o.overrideKey = "instanceSettings";
    return o;
}
ConfigOverlayEditor *open(ConfigStore &main, const ConfigOverlayOptions &options) {
    ConfigOverlayEditor *editor = nullptr;
    const auto connection =
        QObject::connect(&main, &ConfigStore::overlayOpened, &main,
                         [&](quint64, ConfigOverlayEditor *created) { editor = created; });
    const auto result = compat::request(main, [&] { return main.openOverlay(options); });
    QObject::disconnect(connection);
    if (result.status != "opened" || !editor)
        qFatal("overlay open failed: %s", qPrintable(result.error.code));
    return editor;
}
int held(ConfigStore &store) {
    int count = -1;
    QEventLoop loop;
    ConfigStoreTestAccess::run(
        store, [&](ConfigWorker &w) { count = w.heldOverlayLeases(); }, &loop,
        [&] { loop.quit(); });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    if (count < 0)
        qFatal("worker probe timeout");
    return count;
}
int persistedVolume(ConfigStore &store) {
    int value = -1;
    QEventLoop loop;
    ConfigStoreTestAccess::run(
        store,
        [&](ConfigWorker &w) {
            const auto id =
                w.backend()->submit({ConfigBackendRequest::Operation::Read, "main", {}, {}});
            const auto raw = decodeMemoryValue(w.backend()->take(id)->bytes);
            value = raw.value("rows")
                        .toList()[0]
                        .toMap()
                        .value("instanceSettings")
                        .toMap()
                        .value("volume")
                        .toInt();
        },
        &loop, [&] { loop.quit(); });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    return value;
}
} // namespace
class OverlayTest : public QObject {
    Q_OBJECT
private slots:
    void requestLookupsAndReservationsPreserveEndpointFences() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        ConfigRetainedGroupOptions group{
            "same", {{"settings", "volume"}}, {{{"settings", "volume"}, true, 2}}, {}};
        const auto capture = compat::request(base, [&] { return base.retainGroup(group); });
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 4).accepted);
        const auto fence = ConfigStoreTestAccess::endpointRevision(base, {"settings", "volume"});
        QCOMPARE(compat::request(base, [&] { return base.retainGroup(group); }).groupToken,
                 capture.groupToken);
        QCOMPARE(ConfigStoreTestAccess::endpointRevision(base, {"settings", "volume"}), fence);
        ConfigRetainedGroupOptions reservation{
            "reservation", {{"settings", "volume"}}, {}, {capture.groupToken}};
        const auto reserved = compat::request(base, [&] { return base.retainGroup(reservation); });
        QCOMPARE(reserved.status, QString("retained"));
        QCOMPARE(ConfigStoreTestAccess::endpointRevision(base, {"settings", "volume"}), fence);
        QCOMPARE(compat::request(base, [&] { return base.retainGroup(reservation); }).groupToken,
                 reserved.groupToken);
        QCOMPARE(ConfigStoreTestAccess::endpointRevision(base, {"settings", "volume"}), fence);
        ConfigStoreTestAccess::run(base, [](auto &w) {
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::BeforeOperation});
        });
        QCOMPARE(compat::request(base, [&] { return base.saveGroup(capture.groupToken); }).status,
                 QString("failed"));
        ConfigStoreTestAccess::run(base, [](auto &w) {
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::BeforeOperation});
        });
        QCOMPARE(compat::request(base, [&] { return base.retryGroup(capture.groupToken); }).status,
                 QString("failed"));
        QCOMPARE(ConfigStoreTestAccess::endpointRevision(base, {"settings", "volume"}), fence);
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).status, QString("saved"));
        QVERIFY(!editor->conflicted());
        QCOMPARE(persistedVolume(main), 4);
    }
    void openingPendingDiskIntentDoesNotInventForeignGeneration() {
        QTemporaryDir dir;
        const auto file = dir.filePath("main.json");
        const QByteArray bytes("{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"volume\":2}}]}");
        compat::put(file, bytes);
        ConfigStore base(baseOptions()), main(mainOptions(file));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        compat::request(main, [&] {
            return main
                .setUser({"rows"},
                         QVariantList{QVariantMap{
                             {"id", "x"}, {"instanceSettings", QVariantMap{{"volume", 3}}}}})
                .requestId;
        });
        const auto displayed = main.info({"rows"}).publicationGeneration;
        const auto opened = compat::request(
            main, [&] { return main.openOverlay(selection(main, base.scope({"settings"}))); });
        QCOMPARE(opened.status, QString("opened"));
        QCOMPARE(main.info({"rows"}).publicationGeneration, displayed);
        QCOMPARE(compat::bytes(file), bytes);
    }
    void throwingAdmissionSettlesAndReleases() {
        // Isolate the abort if a native exception escapes Qt's dispatcher.
        if (!qEnvironmentVariableIsSet("CONFIGQML_OVERLAY_THROW_CHILD")) {
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("CONFIGQML_OVERLAY_THROW_CHILD", "1");
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(),
                        {"throwingAdmissionSettlesAndReleases"});
            QVERIFY(child.waitForFinished(15000));
            QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                     child.readAllStandardOutput() + child.readAllStandardError());
            return;
        }
        auto options = mainOptions();
        options.admission = [](const ConfigInput &) -> QString {
            throw std::runtime_error("not a public diagnostic");
        };
        ConfigStore base(baseOptions()), main(options);
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        const auto receipt = compat::request(main, [&] { return editor->apply(); });
        QCOMPARE(receipt.status, QString("failed"));
        QCOMPARE(receipt.error.code, QString("worker-exception"));
        QCOMPARE(receipt.refreshState, QString("not_refreshed"));
        QVERIFY(editor->dirty());
        QVERIFY(!editor->busy());
        QCOMPARE(editor->value({"volume"}).toInt(), 7);
        QCOMPARE(main.info({"rows"}).latestInput, main.info({"rows"}).workerCovered);
        QCOMPARE(
            compat::request(base, [&] { return base.setUser({"settings", "volume"}, 8).requestId; })
                .status,
            QString("accepted"));
        QCOMPARE(base.snapshot().values.value({"settings", "volume"}).toInt(), 8);
        QCOMPARE(main.value({"rows"})
                     .toList()[0]
                     .toMap()
                     .value("instanceSettings")
                     .toMap()
                     .value("volume")
                     .toInt(),
                 2);
    }
    void scopeClosureReachesLivingView() {
        for (bool destroy : {false, true}) {
            ConfigStore base(baseOptions()), main(mainOptions());
            QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
            QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
            auto *scope = base.scope({"settings"});
            auto *view = open(main, selection(main, scope, ConfigOverlayMode::View));
            if (destroy)
                delete scope;
            else
                scope->detach();
            QTRY_COMPARE(view->status(), ConfigOverlayEditor::Status::Error);
            QCOMPARE(view->error(), QString("overlay-owner"));
            QCOMPARE(compat::request(
                         base, [&] { return base.setUser({"settings", "volume"}, 8).requestId; })
                         .status,
                     QString("accepted"));
            QCOMPARE(view->value({"volume"}).toInt(), 2);
        }
    }
    void sparseDiskRestartOpaque() {
        QTemporaryDir dir;
        const auto mainFile = dir.filePath("main.json"), baseFile = dir.filePath("base.json");
        compat::put(baseFile, "{\"volume\":1}");
        compat::put(mainFile, "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"volume\":2,"
                              "\"opaque\":{\"keep\":false}}}]}");
        const auto baseBytes = compat::bytes(baseFile);
        ConfigStore base(baseOptions(baseFile)), main(mainOptions(mainFile));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"}, false)));
        QCOMPARE(editor->value({"volume"}).toInt(), 2);
        QVERIFY(!editor->value({"optional"}).isValid());
        QVERIFY(editor->setValue({"volume"}, 1).accepted);
        QVERIFY(editor->setValue({"object"}, QVariantMap{{"a", 7}, {"b", 2}}).accepted);
        QVERIFY(editor->setValue({"list"}, QVariantList{8}).accepted);
        const auto result = compat::request(main, [&] { return editor->apply(); });
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(result.refreshState, QString("current"));
        QVERIFY(!editor->dirty());
        QVERIFY(!editor->conflicted());
        QCOMPARE(compat::bytes(baseFile), baseBytes);
        ConfigStore restarted(mainOptions(mainFile));
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        const auto overrides =
            restarted.value({"rows"}).toList()[0].toMap().value("instanceSettings").toMap();
        QVERIFY(!overrides.contains("volume"));
        QVERIFY(overrides.contains("opaque"));
        QCOMPARE(overrides.value("object").toMap().size(), 2);
        QVERIFY(editor->setValue({"volume"}, 6).accepted);
        QCOMPARE(compat::request(main, [&] { return editor->resetOverrides(); }).status,
                 QString("saved"));
        QVERIFY(!main.value({"rows"}).toList()[0].toMap().contains("instanceSettings"));
    }
    void cancellationAtEitherOrderedLease() {
        for (bool ownerFirst : {false, true})
            for (int action = 0; action < 6; ++action) {
                std::unique_ptr<ConfigStore> base, main;
                if (ownerFirst) {
                    main = std::make_unique<ConfigStore>(mainOptions());
                    base = std::make_unique<ConfigStore>(baseOptions());
                } else {
                    base = std::make_unique<ConfigStore>(baseOptions());
                    main = std::make_unique<ConfigStore>(mainOptions());
                }
                QTRY_COMPARE(base->status(), ConfigStore::Status::Ready);
                QTRY_COMPARE(main->status(), ConfigStore::Status::Ready);
                auto *scope = base->scope({"settings"});
                auto *editor = open(*main, selection(*main, scope));
                QVERIFY(editor->setValue({"volume"}, 7).accepted);
                auto &heldStore = ownerFirst ? *main : *base;
                ConfigStoreTestAccess::run(heldStore,
                                           [](ConfigWorker &w) { w.holdOverlayLeases(true); });
                QSignalSpy results(main.get(), &ConfigStore::completed);
                bool callback = false;
                const auto id = editor->apply(this, [&](const ConfigResult &) { callback = true; });
                QTRY_COMPARE(held(heldStore), 1);
                if (action == 0)
                    scope->detach();
                else if (action == 1)
                    delete scope;
                else if (action == 2)
                    base->close();
                else if (action == 3)
                    base.reset();
                else if (action == 4)
                    editor->detach();
                else
                    delete editor;
                QTRY_VERIFY(!results.isEmpty());
                QCOMPARE(results[0].first().value<ConfigResult>().requestId, id);
                QCOMPARE(results[0].first().value<ConfigResult>().status, QString("failed"));
                QCOMPARE(results[0].first().value<ConfigResult>().error.code,
                         QString("overlay-owner"));
                QCOMPARE(callback, action < 4);
                if (action < 4) {
                    QVERIFY(editor->dirty());
                    QVERIFY(!editor->busy());
                    QCOMPARE(editor->value({"volume"}).toInt(), 7);
                }
                QCOMPARE(
                    compat::request(*main, [&] { return main->setUser({"other"}, 8).requestId; })
                        .status,
                    QString("accepted"));
                QCOMPARE(main->snapshot().values.value({"other"}).toInt(), 8);
            }
    }
    void openingScopeClosesBeforeItsLeaseTurn() {
        ConfigStore main(mainOptions()), base(baseOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        ConfigStoreTestAccess::run(main, [](ConfigWorker &w) { w.holdOverlayLeases(true); });
        auto *scope = base.scope({"settings"});
        QSignalSpy results(&main, &ConfigStore::completed);
        QSignalSpy opened(&main, &ConfigStore::overlayOpened);
        const auto id = main.openOverlay(selection(main, scope));
        QTRY_COMPARE(held(main), 1);
        scope->detach();
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(results[0].first().value<ConfigResult>().requestId, id);
        QCOMPARE(results[0].first().value<ConfigResult>().error.code, QString("overlay-owner"));
        QCOMPARE(opened.size(), 0);
        QTRY_COMPARE(held(main), 0);
    }
    void ownerClosesOrFailsDuringHandshake() {
        for (int failure = 0; failure < 3; ++failure) {
            ConfigStore base(baseOptions()), main(mainOptions());
            QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
            QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
            auto *editor = open(main, selection(main, base.scope({"settings"})));
            QVERIFY(editor->setValue({"volume"}, 7).accepted);
            ConfigStoreTestAccess::run(base, [](ConfigWorker &w) { w.holdOverlayLeases(true); });
            QSignalSpy results(&main, &ConfigStore::completed);
            const auto id = editor->apply();
            QTRY_COMPARE(held(base), 1);
            if (failure == 2)
                ConfigStoreTestAccess::run(main, [](ConfigWorker &w) {
                    w.overlayControl([](ConfigWorker &) { throw std::runtime_error("fixture"); });
                });
            else if (failure == 1)
                ConfigStoreTestAccess::run(main, [](ConfigWorker &w) { w.fail("fixture-owner"); });
            else
                main.close();
            QTRY_VERIFY(!results.isEmpty());
            QCOMPARE(results[0].first().value<ConfigResult>().requestId, id);
            QCOMPARE(results[0].first().value<ConfigResult>().status, QString("failed"));
            QVERIFY(!editor->busy());
            QVERIFY(editor->dirty());
            QCOMPARE(editor->value({"volume"}).toInt(), 7);
            QCOMPARE(compat::request(
                         base, [&] { return base.setUser({"settings", "volume"}, 8).requestId; })
                         .status,
                     QString("accepted"));
        }
    }
    void destructionAfterAdmissionKeepsRequestedPersistence() {
        QTemporaryDir dir;
        const auto path = dir.filePath("main.json");
        for (bool destroyOwner : {false, true}) {
            compat::put(path, "{\"rows\":[{\"id\":\"x\"}]}");
            // Delay memory publication of a disjoint route before the JSON
            // owning route. Destroying delivery cannot cancel the admitted save.
            auto options = mainOptions(path);
            // Source prefixes may not overlap in one layer; put the memory
            // disjoint source and JSON route under separate declared groups.
            options.schema = ConfigSchema(
                D::group({{"group", *mainOptions().schema.descriptor({})},
                          {"auxiliary", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
            options.layers[0].sources = {{"a-memory", {"auxiliary"}, {}}, {"main", {"group"}, {}}};
            ConfigStore base(baseOptions());
            auto main = std::make_unique<ConfigStore>(options);
            QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
            QTRY_COMPARE(main->status(), ConfigStore::Status::Ready);
            compat::request(*main, [&] { return main->setUser({"auxiliary", "n"}, 1).requestId; });
            auto selected = selection(*main, base.scope({"settings"}));
            selected.ownerPath = {"group", "rows"};
            selected.ownerGeneration = main->info(selected.ownerPath).publicationGeneration;
            auto *editor = open(*main, selected);
            QVERIFY(editor->setValue({"volume"}, 7).accepted);
            ConfigStoreTestAccess::run(*main, [](ConfigWorker &w) {
                w.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
            });
            bool called = false;
            editor->apply(this, [&](const ConfigResult &) { called = true; });
            QTRY_COMPARE(main->value({"group", "rows"})
                             .toList()[0]
                             .toMap()
                             .value("instanceSettings")
                             .toMap()
                             .value("volume")
                             .toInt(),
                         7);
            // Retain only a test transport to release the held worker after
            // consumer destruction; it owns no QObject or settings graph.
            auto release = ConfigStoreTestAccess::backendRelease(*main);
            if (destroyOwner)
                main.reset();
            else
                delete editor;
            release();
            QTRY_VERIFY(compat::bytes(path).contains("7"));
            QVERIFY(!called);
            ConfigStore restarted(mainOptions(path));
            QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
            auto *reopened = open(restarted, selection(restarted, base.scope({"settings"})));
            QCOMPARE(reopened->value({"volume"}).toInt(), 7);
        }
    }
    void equalIntentConflictAndDiscard() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 9).accepted);
        compat::request(base, [&] { return base.setUser({"settings", "volume"}, 1).requestId; });
        const auto result = compat::request(main, [&] { return editor->apply(); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("overlay-conflict"));
        QVERIFY(editor->dirty());
        QVERIFY(editor->conflicted());
        QCOMPARE(editor->value({"volume"}).toInt(), 9);
        QCOMPARE(main.value({"rows"})
                     .toList()[0]
                     .toMap()
                     .value("instanceSettings")
                     .toMap()
                     .value("volume")
                     .toInt(),
                 2);
        QCOMPARE(compat::request(main, [&] { return editor->discard(); }).status,
                 QString("discarded"));
        QVERIFY(!editor->dirty());
        QVERIFY(!editor->conflicted());
        QCOMPARE(editor->value({"volume"}).toInt(), 2);
    }
    void maskedInvalidAndABAInputsFenceTheBase() {
        for (int action = 0; action < 4; ++action) {
            QTemporaryDir dir;
            const auto file = dir.filePath("base.json");
            compat::put(file, "{\"volume\":1}");
            auto options = baseOptions(file);
            options.layers.append({"environment",
                                   ConfigLayerKind::Environment,
                                   ConfigAdmissionPolicy::FieldFallback,
                                   {{"environment", {"settings"}, {{"volume", 1}}}},
                                   false});
            options.layers.append({"runtime",
                                   ConfigLayerKind::Runtime,
                                   ConfigAdmissionPolicy::FieldFallback,
                                   {{"runtime", {}, {}}},
                                   false});
            ConfigStore base(options), main(mainOptions());
            QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
            QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
            auto *editor = open(main, selection(main, base.scope({"settings"})));
            QVERIFY(editor->setValue({"volume"}, 7).accepted);
            if (action == 0) {
                compat::request(base,
                                [&] { return base.setUser({"settings", "volume"}, 4).requestId; });
                QCOMPARE(base.value({"settings", "volume"}).toInt(), 1);
                QCOMPARE(base.info({"settings", "volume"}).provenance.maskedBy,
                         QString("environment"));
            } else if (action == 1) {
                compat::request(
                    base, [&] { return base.setRuntime({"settings", "volume"}, 1).requestId; });
                QCOMPARE(base.value({"settings", "volume"}).toInt(), 1);
            } else if (action == 2) {
                compat::put(file, "{\"volume\":\"schema-invalid\"}");
            } else {
                compat::request(base,
                                [&] { return base.setUser({"settings", "volume"}, 4).requestId; });
                compat::request(base,
                                [&] { return base.setUser({"settings", "volume"}, 1).requestId; });
            }
            QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                     QString("overlay-conflict"));
            QVERIFY(editor->dirty());
            QVERIFY(editor->conflicted());
            QCOMPARE(editor->value({"volume"}).toInt(), 7);
            QCOMPARE(main.value({"rows"})
                         .toList()[0]
                         .toMap()
                         .value("instanceSettings")
                         .toMap()
                         .value("volume")
                         .toInt(),
                     2);
        }
    }
    void liveAddressFollowsButEditorNeverRetargets() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *scope = base.scope({"settings"});
        auto *view = open(main, selection(main, scope, ConfigOverlayMode::View));
        auto *editor = open(main, selection(main, scope));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        auto row = [](int volume) {
            return QVariantMap{{"id", "x"}, {"instanceSettings", QVariantMap{{"volume", volume}}}};
        };
        for (const auto &rows :
             {QVariantList{row(3), row(2)}, QVariantList{row(4)}, QVariantList{}}) {
            compat::request(main, [&] { return main.setUser({"rows"}, rows).requestId; });
            if (!rows.isEmpty()) {
                QTRY_COMPARE(
                    view->value({"volume"}).toInt(),
                    rows[0].toMap().value("instanceSettings").toMap().value("volume").toInt());
            } else {
                QTRY_COMPARE(view->status(), ConfigOverlayEditor::Status::Error);
                QCOMPARE(view->error(), QString("overlay-identity"));
            }
            QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                     QString("overlay-conflict"));
            QCOMPARE(main.value({"rows"}).toList(), rows);
            QVERIFY(editor->dirty());
            QCOMPARE(editor->value({"volume"}).toInt(), 7);
        }
    }
    void failedResetRetainsExplicitDirtyIntent() {
        QTemporaryDir dir;
        const auto file = dir.filePath("main.json");
        const QByteArray bytes("{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"volume\":2}}]}");
        compat::put(file, bytes);
        auto options = mainOptions(file);
        options.fileFault = [](const QString &phase, const QString &) {
            return phase == "write" ? QString("fixture-write") : QString();
        };
        ConfigStore base(baseOptions()), main(options);
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(!editor->dirty());
        const auto failed = compat::request(main, [&] { return editor->resetOverrides(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.refreshState, QString("not_refreshed"));
        QVERIFY(editor->dirty());
        QVERIFY(!editor->busy());
        QCOMPARE(editor->value({"volume"}).toInt(), 2);
        QCOMPARE(compat::bytes(file), bytes);
    }
    void liveViewFrozenEditorAndReadOnly() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *scope = base.scope({"settings"});
        auto *view = open(main, selection(main, scope, ConfigOverlayMode::View));
        auto *editor = open(main, selection(main, scope));
        QVERIFY(editor->setValue({"volume"}, 9).accepted);
        QVERIFY(!view->setValue({"volume"}, 9).accepted);
        QVERIFY(!view->unset({"volume"}));
        QCOMPARE(compat::request(main, [&] { return view->apply(); }).error.code,
                 QString("overlay-readonly"));
        compat::request(
            base, [&] { return base.setUser({"settings", "optional"}, QString("new")).requestId; });
        QTRY_COMPARE(view->value({"optional"}).toString(), QString("new"));
        QVERIFY(!editor->value({"optional"}).isValid());
        QTRY_VERIFY(editor->conflicted());
        QCOMPARE(editor->value({"volume"}).toInt(), 9);
        scope->detach();
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                 QString("overlay-owner"));
        QVERIFY(editor->dirty());
    }
    void placementAndOptions() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto options = selection(main, base.scope({"settings"}));
        options.identityValue = QString("different");
        QCOMPARE(compat::request(main, [&] { return main.openOverlay(options); }).error.code,
                 QString("overlay-identity"));
        options = selection(main, main.scope({}));
        QCOMPARE(compat::request(main, [&] { return main.openOverlay(options); }).error.code,
                 QString("overlay-cycle"));
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        const auto rows = main.value({"rows"});
        compat::request(main, [&] { return main.setUser({"rows"}, rows).requestId; });
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                 QString("overlay-conflict"));
        QVERIFY(editor->dirty());
        QVERIFY(!editor->setValue({"volume"}, QString("wrong")).accepted);
        QVERIFY(!editor->setValue({}, QVariantMap{{"volume", 3}}).accepted);
    }
    void postAdmissionChangeKeepsReceiptAndPreview() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        ConfigStoreTestAccess::run(main, [](ConfigWorker &w) {
            w.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
        });
        QSignalSpy results(&main, &ConfigStore::completed);
        const auto id = editor->apply();
        QTRY_COMPARE(main.value({"rows"})
                         .toList()[0]
                         .toMap()
                         .value("instanceSettings")
                         .toMap()
                         .value("volume")
                         .toInt(),
                     7);
        compat::request(base, [&] { return base.setUser({"settings", "volume"}, 4).requestId; });
        ConfigStoreTestAccess::run(main, [](ConfigWorker &w) { w.releaseBackend(); });
        QTRY_VERIFY(!results.isEmpty());
        QCOMPARE(results.last().first().value<ConfigResult>().requestId, id);
        const auto receipt = results.last().first().value<ConfigResult>();
        QCOMPARE(receipt.status, QString("saved"));
        QCOMPARE(receipt.refreshState, QString("stale"));
        QVERIFY(editor->dirty());
        QVERIFY(editor->conflicted());
        QCOMPARE(editor->value({"volume"}).toInt(), 7);
    }
    void admittedSaveNeverCoversOrErasesLaterMainInput() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        ConfigStoreTestAccess::run(main, [](ConfigWorker &w) {
            w.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
        });
        QSignalSpy results(&main, &ConfigStore::completed);
        const auto id = editor->apply();
        QTRY_COMPARE(main.snapshot()
                         .values.value({"rows"})
                         .toList()[0]
                         .toMap()
                         .value("instanceSettings")
                         .toMap()
                         .value("volume")
                         .toInt(),
                     7);
        const auto suffix = main.setUser(
            {"rows"}, QVariantList{QVariantMap{{"id", "x"},
                                               {"instanceSettings", QVariantMap{{"volume", 42}}}}});
        QVERIFY(suffix.accepted);
        ConfigStoreTestAccess::run(main, [](ConfigWorker &w) { w.releaseBackend(); });
        QTRY_VERIFY(results.size() >= 2);
        const auto receipt = results[0].first().value<ConfigResult>();
        QCOMPARE(receipt.requestId, id);
        QCOMPARE(receipt.status, QString("saved"));
        QCOMPARE(receipt.refreshState, QString("stale"));
        QVERIFY(receipt.inputPrefix < suffix.inputSequence);
        for (const auto &coverage : receipt.coverage)
            QVERIFY(coverage.inputThrough < suffix.inputSequence);
        QCOMPARE(persistedVolume(main), 7);
        QTRY_COMPARE(main.snapshot()
                         .values.value({"rows"})
                         .toList()[0]
                         .toMap()
                         .value("instanceSettings")
                         .toMap()
                         .value("volume")
                         .toInt(),
                     42);
        QVERIFY(editor->dirty());
        QVERIFY(editor->conflicted());
        QCOMPARE(editor->value({"volume"}).toInt(), 7);
        QCOMPARE(compat::request(main, [&] { return main.save(); }).status, QString("saved"));
        QCOMPARE(persistedVolume(main), 42);
    }
    void leasesUnrelatedProgressLateMainAndBusy() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        ConfigStoreTestAccess::run(base, [](ConfigWorker &w) { w.holdOverlayLeases(true); });
        QSignalSpy results(&main, &ConfigStore::completed);
        const auto applyId = editor->apply();
        QTRY_COMPARE(held(base), 1);
        QVERIFY(!editor->setValue({"volume"}, 8).accepted);
        QVERIFY(!editor->unset({"volume"}));
        const auto refused = editor->discard();
        const auto unrelated = main.setUser({"other"}, 8);
        QVERIFY(unrelated.accepted);
        QCOMPARE(
            compat::request(base, [&] { return base.setUser({"extra", "n"}, 3).requestId; }).status,
            QString("accepted"));
        QTRY_COMPARE(main.snapshot().values.value({"other"}).toInt(), 8);
        QVERIFY(main.snapshot().inputPrefix < unrelated.inputSequence);
        const auto rows = main.value({"rows"}).toList();
        auto newer = rows;
        auto row = newer[0].toMap();
        row["instanceSettings"] = QVariantMap{{"volume", 42}};
        newer[0] = row;
        QVERIFY(main.setUser({"rows"}, newer).accepted);
        ConfigStoreTestAccess::run(base, [](ConfigWorker &w) { w.holdOverlayLeases(false); });
        QTRY_VERIFY(results.size() >= 4);
        QCOMPARE(results[0].first().value<ConfigResult>().requestId, applyId);
        QCOMPARE(results[0].first().value<ConfigResult>().error.code, QString("overlay-conflict"));
        QCOMPARE(results[1].first().value<ConfigResult>().requestId, refused);
        QCOMPARE(results[1].first().value<ConfigResult>().error.code, QString("overlay-busy"));
        QCOMPARE(main.value({"rows"})
                     .toList()[0]
                     .toMap()
                     .value("instanceSettings")
                     .toMap()
                     .value("volume")
                     .toInt(),
                 42);
        QVERIFY(editor->dirty());
        QCOMPARE(editor->value({"volume"}).toInt(), 7);
        QCOMPARE(compat::request(main, [&] { return main.save(); }).status, QString("saved"));
    }
    void handshakeCancellationAndDeadConsumer() {
        ConfigStore base(baseOptions()), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        ConfigStoreTestAccess::run(base, [](ConfigWorker &w) { w.holdOverlayLeases(true); });
        QSignalSpy results(&main, &ConfigStore::completed);
        const auto id = editor->apply();
        QTRY_COMPARE(held(base), 1);
        ConfigStoreTestAccess::run(base, [](ConfigWorker &w) { w.fail("fixture-owner-failure"); });
        QTRY_VERIFY(!results.isEmpty());
        QCOMPARE(results[0].first().value<ConfigResult>().requestId, id);
        QCOMPARE(results[0].first().value<ConfigResult>().error.code, QString("overlay-owner"));
        QVERIFY(editor->dirty());
        QTRY_COMPARE(held(base), 0);
        QSignalSpy opened(&main, &ConfigStore::overlayOpened);
        auto *context = new QObject;
        bool called = false;
        main.openOverlay(selection(main, base.scope({"settings"})), context,
                         [&](const ConfigResult &) { called = true; });
        delete context;
        compat::request(main, [&] { return main.reload(); });
        QVERIFY(!called);
        QCOMPARE(opened.size(), 0);
    }
    void foreignIdentityMaskedDraftAndProfileFences() {
        QTemporaryDir dir;
        const auto file = dir.filePath("base.json");
        compat::put(file, "{\"volume\":1}");
        ConfigStore base(baseOptions(file)), main(mainOptions());
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *scope = base.scope({"settings"});
        auto *editor = open(main, selection(main, scope));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        QSaveFile replacement(file);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QVERIFY(replacement.write("{\"volume\":1}") > 0);
        QVERIFY(replacement.commit());
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                 QString("overlay-conflict"));
        QCOMPARE(compat::request(main, [&] { return editor->discard(); }).status,
                 QString("discarded"));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        auto *draft = base.draft({{"settings"}});
        QVERIFY(draft->setValue({"settings", "volume"}, 1).accepted);
        QVERIFY(compat::request(base, [&] { return draft->apply(); }).status != "failed");
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                 QString("overlay-conflict"));
        QCOMPARE(compat::request(main, [&] { return editor->discard(); }).status,
                 QString("discarded"));
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        const auto profile = dir.filePath("profile.json");
        compat::put(profile, "{\"format\":1,\"name\":\"same\",\"entries\":[{\"path\":[\"settings\","
                             "\"volume\"],\"present\":true,\"value\":1}]}");
        QVERIFY(compat::request(base, [&] { return base.importProfile(profile); }).status !=
                "failed");
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                 QString("overlay-conflict"));
    }
    void normalizedNestedRowsDoNotMaterialize() {
        for (bool screenShape : {false, true}) {
            auto o = mainOptions();
            const auto row = o.schema.descriptor({"rows"})->element.first();
            auto monitor = D::leaf(T::Record, QVariantMap{{"widgets", QVariantList{}}});
            monitor.fields = {{"widgets", D::list(row, {})}};
            if (screenShape) {
                auto widgets = D::leaf(T::Record, QVariantMap{{"left", QVariantList{}}});
                widgets.fields = {{"left", D::list(row, {})}, {"right", D::list(row, {})}};
                widgets.fields["right"].optional = true;
                widgets.fields["right"].defaultValue.reset();
                monitor.fields["widgets"] = widgets;
                monitor.defaultValue =
                    QVariantMap{{"widgets", QVariantMap{{"left", QVariantList{}}}}};
            }
            o.schema = ConfigSchema(D::group({{"rows", D::list(monitor, {})}}));
            const QVariantList widgets{QVariantMap{{"id", "hidden"}},
                                       QVariantMap{{"id", "x"}, {"deprecated", 3}},
                                       QVariantMap{{"id", "x"}, {"deprecated", 4}}};
            o.layers[0].sources[0].raw = {
                {"rows", QVariantList{QVariantMap{
                             {"widgets", screenShape ? QVariant(QVariantMap{{"left", widgets}})
                                                     : QVariant(widgets)}}}}};
            ConfigRecordListPolicySpec policy;
            policy.policyId = "widgets";
            policy.path = {"rows"};
            policy.listPath = {{ConfigValueSelector::Kind::Each},
                               {ConfigValueSelector::Kind::Key, "widgets"}};
            if (screenShape)
                policy.listPath.append({ConfigValueSelector::Kind::Key, "left"});
            policy.keyField = "id";
            policy.preservedFields = {"instanceSettings"};
            policy.pruneUnknown = policy.normalizeFields = true;
            o.recordListPolicies = {policy};
            ConfigStore base(baseOptions()), main(o);
            QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
            QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
            ConfigRecordListMetadata metadata;
            metadata.ready = true;
            metadata.entries = {{"x", true, false, true, {{"injected", false}}}};
            QCOMPARE(compat::request(
                         main, [&] { return main.updateRecordListPolicy("widgets", metadata); })
                         .status,
                     QString("projected"));
            auto selected = selection(main, base.scope({"settings"}));
            selected.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0},
                                  {ConfigValueSelector::Kind::Key, "widgets"}};
            if (screenShape)
                selected.entryPath.append({ConfigValueSelector::Kind::Key, "left"});
            selected.entryPath.append({ConfigValueSelector::Kind::Index, {}, 0});
            auto *editor = open(main, selected);
            QVERIFY(editor->setValue({"volume"}, 7).accepted);
            QCOMPARE(compat::request(main, [&] { return editor->apply(); }).status,
                     QString("saved"));
            auto persisted = std::make_shared<QVariantList>();
            bool inspected = false;
            ConfigStoreTestAccess::run(
                main,
                [persisted, screenShape](ConfigWorker &w) {
                    const auto widgets = w.layers()
                                             ->source("user", "main")
                                             ->raw.value("rows")
                                             .toList()[0]
                                             .toMap()
                                             .value("widgets");
                    *persisted =
                        screenShape ? widgets.toMap().value("left").toList() : widgets.toList();
                    if (screenShape && widgets.toMap().contains("right"))
                        persisted->clear(); // The missing optional list must stay missing.
                },
                this, [&] { inspected = true; });
            QTRY_VERIFY(inspected);
            QCOMPARE(persisted->size(), 3);
            QCOMPARE((*persisted)[0].toMap().value("id").toString(), QString("hidden"));
            QCOMPARE((*persisted)[1].toMap().value("deprecated").toInt(), 3);
            QVERIFY(!(*persisted)[1].toMap().contains("injected"));
            QCOMPARE(
                (*persisted)[1].toMap().value("instanceSettings").toMap().value("volume").toInt(),
                7);
            QVERIFY(!(*persisted)[2].toMap().contains("instanceSettings"));
            QVERIFY(editor->setValue({"volume"}, 8).accepted);
            compat::request(main, [&] { return main.updateRecordListPolicy("widgets", metadata); });
            QCOMPARE(compat::request(main, [&] { return editor->apply(); }).error.code,
                     QString("overlay-conflict"));
        }
    }
    void endpointCyclesAndDetachReleasesDependence() {
        auto common = mainOptions();
        auto root = *common.schema.descriptor({});
        root.fields["settings"] = *baseOptions().schema.descriptor({"settings"});
        common.schema = ConfigSchema(root);
        ConfigStore a(common), b(common);
        QTRY_COMPARE(a.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(b.status(), ConfigStore::Status::Ready);
        auto *editor = open(a, selection(a, b.scope({"settings"})));
        const auto opposite = selection(b, a.scope({"settings"}));
        QCOMPARE(compat::request(b, [&] { return b.openOverlay(opposite); }).error.code,
                 QString("overlay-cycle"));
        editor->detach();
        auto *reverse = open(b, opposite);
        QVERIFY(reverse->ready());
        reverse->detach();
        QCOMPARE(compat::request(a, [&] { return a.save(); }).status, QString("nothing_to_save"));
    }
    void opaquePrototypeAndWideDomains() {
        QTemporaryDir dir;
        auto o = baseOptions();
        auto descriptor = *o.schema.descriptor({});
        descriptor.fields["settings"].fields["wide"] = D::leaf(
            T::UnsignedInteger, QVariant::fromValue(std::numeric_limits<quint64>::max() - 1));
        o.schema = ConfigSchema(descriptor);
        const auto file = dir.filePath("main.json");
        compat::put(file, "{\"outside\":true,\"rows\":[{\"id\":\"x\",\"instanceSettings\":{"
                          "\"wide\":18446744073709551615,\"__proto__\":{\"keep\":false},\"opaque\":"
                          "{\"number\":18446744073709551615}}}]}");
        ConfigStore base(o), main(mainOptions(file));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        auto *editor = open(main, selection(main, base.scope({"settings"})));
        QCOMPARE(editor->value({"wide"}).toULongLong(), std::numeric_limits<quint64>::max());
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).status, QString("saved"));
        QVERIFY(compat::bytes(file).contains("18446744073709551615"));
        ConfigStore restarted(mainOptions(file));
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        const auto saved =
            restarted.value({"rows"}).toList()[0].toMap().value("instanceSettings").toMap();
        QVERIFY(saved.contains("__proto__"));
        QVERIFY(saved.contains("opaque"));
        auto *reopened = open(restarted, selection(restarted, base.scope({"settings"})));
        QCOMPARE(reopened->value({"wide"}).toULongLong(), std::numeric_limits<quint64>::max());
        QVERIFY(!reopened->schema().descriptor({"opaque"}));
    }
    void partialIOKeepsDirtyAndActualRouteFacts() {
        QTemporaryDir dir;
        auto options = mainOptions();
        const auto rows = options.layers[0].sources[0].raw;
        options.schema =
            ConfigSchema(D::group({{"group", *options.schema.descriptor({})},
                                   {"extra", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        options.layers[0].sources = {{"main", {"group"}, rows}, {"aux", {"extra"}, {}}};
        options.jsonFiles = {{"user", "main", dir.filePath("main.json")},
                             {"user", "aux", dir.filePath("aux.json")}};
        compat::put(dir.filePath("main.json"), "{\"rows\":[{\"id\":\"x\"}]}");
        options.fileFault = [](const QString &phase, const QString &path) {
            return phase == "write" && path.endsWith("aux.json") ? QString("fixture-write")
                                                                 : QString();
        };
        ConfigStore base(baseOptions()), main(options);
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        compat::request(main, [&] { return main.setUser({"extra", "n"}, 8).requestId; });
        auto selected = selection(main, base.scope({"settings"}));
        selected.ownerPath = {"group", "rows"};
        selected.ownerGeneration = main.info(selected.ownerPath).publicationGeneration;
        auto *editor = open(main, selected);
        QVERIFY(editor->setValue({"volume"}, 7).accepted);
        const auto result = compat::request(main, [&] { return editor->apply(); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("partial"));
        QCOMPARE(result.refreshState, QString("not_refreshed"));
        QVERIFY(result.coveredRevisions.contains("main"));
        QVERIFY(!result.coveredRevisions.contains("aux"));
        QVERIFY(editor->dirty());
        QCOMPARE(editor->value({"volume"}).toInt(), 7);
        const auto raw =
            ConfigJsonCodec{}.decode(compat::bytes(dir.filePath("main.json")), "main").raw;
        QVERIFY(compat::bytes(dir.filePath("main.json")).contains("7"));
        QVERIFY(raw.contains("rows"));
        QVERIFY(!QFileInfo::exists(dir.filePath("aux.json")));
    }
};
QTEST_GUILESS_MAIN(OverlayTest)
#include "test_overlay_editor.moc"
