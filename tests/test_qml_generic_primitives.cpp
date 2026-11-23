// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "compatfixture.h"
#include "storeprobe.h"
#include "configpendingreply_p.h"
#include "configqmlpendingreply.h"
#include <QCoreApplication>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <cstdio>

using namespace configqml;
class QmlGenericPrimitivesTest : public QObject {
    Q_OBJECT
private slots:
    void shardedMainEffectCompletion() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        compat::put(directory.filePath("settings.json"), "{}");
        compat::put(
            directory.filePath("bar.json"),
            "{\"widgets\":{\"left\":[{\"id\":\"x\",\"instanceSettings\":{\"opaque\":\"selected\"}},"
            "{\"id\":\"x\",\"instanceSettings\":{\"idleSeconds\":600,\"opaque\":\"sibling\"}},"
            "{\"id\":\"x\",\"instanceSettings\":{\"opaque\":\"inherited\"}}]}}");
        compat::put(directory.filePath("idle.json"),
                    "{\"customCommands\":\"[]\",\"opaque\":false}");
        compat::put(directory.filePath("hooks.json"),
                    "{\"screenLock\":\"\",\"screenUnlock\":\"\"}");
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(OVERLAY_COMPLETION_FIXTURE));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"directory", directory.path()}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY2(root->property("ready").toBool(),
                     qPrintable(root->property("startupError").toString()));
        QSignalSpy setup(root.get(), SIGNAL(setupFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setup"));
        QTRY_COMPARE(setup.size(), 1);
        QCOMPARE(root->property("failure").toString(), QString());
        QSignalSpy quit(root.get(), SIGNAL(quitFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "quit"));
        QTRY_COMPARE(quit.size(), 1);
        QSignalSpy effects(root.get(), SIGNAL(effectsFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "effects", Q_ARG(bool, false)));
        QTRY_COMPARE(effects.size(), 1);
        auto *owner = root->property("owner").value<ConfigQmlStore *>();
        QVERIFY(owner);
        ConfigStoreTestAccess::run(*owner->core(), [](auto &w) { w.holdInputs(true); });
        QSignalSpy restored(root.get(), SIGNAL(restoreFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "restore"));
        QCOMPARE(root->property("pending").toInt(), 1);
        QVERIFY(root->property("boundEnabled").toBool());
        ConfigStoreTestAccess::run(*owner->core(), [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE_WITH_TIMEOUT(restored.size(), 1, 3000);
        QTRY_COMPARE(effects.size(), 2);
        QCOMPARE(root->property("pending").toInt(), 0);
        QCOMPARE(root->property("restoredBinding").toInt(), 1);
        QCOMPARE(root->property("failure").toString(), QString());
        QCOMPARE(engine.toScriptValue(root->property("requests")).property("length").toInt(), 4);
        QCOMPARE(engine.toScriptValue(root->property("callbacks")).toVariant(),
                 engine.toScriptValue(root->property("requests")).toVariant());
        const auto requests = engine.toScriptValue(root->property("requests"));
        QVariantList expectedSignals;
        for (int i = 1; i <= requests.property(3).toString().toInt(); ++i)
            expectedSignals.append(QString::number(i));
        QCOMPARE(engine.toScriptValue(root->property("signalsSeen")).toVariant().toList(),
                 expectedSignals);
        const auto bar =
            ConfigJsonCodec().decode(compat::bytes(directory.filePath("bar.json")), "bar").raw;
        const auto expectedBar =
            ConfigJsonCodec()
                .decode("{\"widgets\":{\"left\":[{\"id\":\"x\",\"instanceSettings\":{\"opaque\":"
                        "\"selected\",\"idleSeconds\":450}},"
                        "{\"id\":\"x\",\"instanceSettings\":{\"idleSeconds\":600,\"opaque\":"
                        "\"sibling\"}},"
                        "{\"id\":\"x\",\"instanceSettings\":{\"opaque\":\"inherited\"}}]}}",
                        "bar")
                .raw;
        QCOMPARE(bar, expectedBar);
        QCOMPARE(
            ConfigJsonCodec().decode(compat::bytes(directory.filePath("idle.json")), "idle").raw,
            (QVariantMap{{"customCommands", "[1]"}, {"opaque", false}}));
        QCOMPARE(
            ConfigJsonCodec().decode(compat::bytes(directory.filePath("hooks.json")), "hooks").raw,
            (QVariantMap{{"screenLock", "kill"}, {"screenUnlock", "kill"}}));
        QCOMPARE(compat::bytes(directory.filePath("settings.json")), QByteArray("{}"));
        QMap<QString, QByteArray> persisted;
        for (const auto &name : {"settings.json", "bar.json", "idle.json", "hooks.json"})
            persisted[name] = compat::bytes(directory.filePath(name));
        QCOMPARE(compat::request(*owner->core(), [&] { return owner->core()->close(); }).status,
                 QString("nothing_to_save"));
        root.reset();
        QQmlEngine coldEngine;
        coldEngine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent coldComponent(&coldEngine, QUrl::fromLocalFile(OVERLAY_COMPLETION_FIXTURE));
        std::unique_ptr<QObject> cold(coldComponent.createWithInitialProperties(
            {{"directory", directory.path()}, {"readOnlySetup", true}}));
        QVERIFY2(cold, qPrintable(coldComponent.errorString()));
        QTRY_VERIFY(cold->property("ready").toBool());
        QSignalSpy coldSetup(cold.get(), SIGNAL(setupFinished()));
        QVERIFY(QMetaObject::invokeMethod(cold.get(), "setup"));
        QTRY_COMPARE(coldSetup.size(), 1);
        QVERIFY(cold->property("boundEnabled").toBool());
        QCOMPARE(cold->property("failure").toString(), QString());
        for (auto it = persisted.cbegin(); it != persisted.cend(); ++it)
            QCOMPARE(compat::bytes(directory.filePath(it.key())), it.value());
    }
    void pendingReplyImmediateConnectionAndRetention() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(ConfigDescriptor::group(
            {{"count", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 3)}}));
        auto store = std::make_unique<ConfigStore>(options);
        QTRY_COMPARE(store->status(), ConfigStore::Status::Ready);
        QQmlEngine engine;
        auto *native = ConfigPendingReplyState::read(store->readProjection());
        auto *reply = new ConfigQmlPendingReply(native, &engine);
        QPointer<ConfigQmlPendingReply> lifetime(reply);
        engine.globalObject().setProperty("reply", engine.newQObject(reply));
        QQmlComponent binding(&engine);
        binding.setData(R"QML(
            import QtQml
            QtObject {
                required property var reply
                readonly property var count: reply.isFinished && !reply.isError
                    ? reply.value.snapshot.state(["count"]).value : -1
            }
        )QML",
                        QUrl("file:pending-reply-binding.qml"));
        QVERIFY2(binding.isReady(), qPrintable(binding.errorString()));
        std::unique_ptr<QObject> bound(
            binding.createWithInitialProperties({{"reply", QVariant::fromValue(reply)}}));
        QVERIFY2(bound, qPrintable(binding.errorString()));
        QCOMPARE(bound->property("count").toInt(), -1);
        const auto connected = engine.evaluate(R"JS(
            var count = 0;
            var captured = null;
            var failure = "";
            reply.finished.connect(function() {
                count++;
                if (!reply.isFinished || reply.isError) failure = "reply outcome";
                captured = reply.value;
                if (captured.snapshot.state(["count"]).value !== 3) failure = "snapshot";
                if (captured.pending.length !== 0 || captured.observations.length !== 0)
                    failure = "read metadata";
                if (captured.write !== undefined) failure = "invented write";
            });
        )JS");
        QVERIFY2(!connected.isError(), qPrintable(connected.toString()));
        QVERIFY(!reply->isFinished());
        QTRY_COMPARE(engine.globalObject().property("count").toInt(), 1);
        QCOMPARE(engine.globalObject().property("failure").toString(), QString());
        QCOMPARE(bound->property("count").toInt(), 3);
        // Reply and snapshot survive their transient source presentation.
        store.reset();
        QVERIFY(lifetime);
        QCOMPARE(engine.evaluate("captured.snapshot.state(['count']).value").toInt(), 3);
        bound.reset();
        engine.globalObject().setProperty("reply", QJSValue(QJSValue::UndefinedValue));
        engine.collectGarbage();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!lifetime);
        QCOMPARE(engine.evaluate("captured.snapshot.state(['count']).value").toInt(), 3);
    }
    void pendingReplyRefusalAndGarbageCollection() {
        QQmlEngine engine;
        for (int i = 0; i < 64; ++i) {
            auto *native = ConfigPendingReplyState::refuse("unsupported-operation", "selection");
            QPointer<ConfigPendingReply> core(native);
            auto *reply = new ConfigQmlPendingReply(native, &engine);
            QPointer<ConfigQmlPendingReply> wrapper(reply);
            engine.globalObject().setProperty("reply", engine.newQObject(reply));
            const auto connected = engine.evaluate(R"JS(
                var failure = "";
                reply.finished.connect(function() {
                    if (!reply.isFinished || !reply.isError
                        || reply.error.code !== "unsupported-operation"
                        || reply.error.phase !== "selection") failure = "refusal";
                });
            )JS");
            QVERIFY2(!connected.isError(), qPrintable(connected.toString()));
            QVERIFY(!reply->isFinished());
            QTRY_VERIFY(reply->isFinished());
            QCOMPARE(engine.globalObject().property("failure").toString(), QString());
            engine.globalObject().setProperty("reply", QJSValue(QJSValue::UndefinedValue));
            engine.collectGarbage();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QTRY_VERIFY(!wrapper);
            QVERIFY(!core);
        }
    }
    void pendingReplyEngineLoss() {
        auto engine = std::make_unique<QQmlEngine>();
        auto *native = ConfigPendingReplyState::refuse("unsupported-operation", "selection");
        auto *reply = new ConfigQmlPendingReply(native, engine.get());
        QPointer<ConfigQmlPendingReply> lifetime(reply);
        engine->globalObject().setProperty("reply", engine->newQObject(reply));
        engine.reset(); // Queued terminal delivery must not reach a dead engine/context.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!lifetime);
    }
    void pendingReplyCollectionDuringHeldRead() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(ConfigDescriptor::group(
            {{"count", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 3)}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QQmlEngine engine;
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        auto *native = ConfigPendingReplyState::read(store.readProjection());
        auto *reply = new ConfigQmlPendingReply(native, &engine);
        QPointer<ConfigQmlPendingReply> lifetime(reply);
        engine.globalObject().setProperty("reply", engine.newQObject(reply));
        QTRY_COMPARE(held.size(), 1);
        QVERIFY(!reply->isFinished());
        engine.globalObject().setProperty("reply", QJSValue(QJSValue::UndefinedValue));
        engine.collectGarbage();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!lifetime);
        ConfigStoreTestAccess::releaseReader(store, held);
        QCoreApplication::processEvents();
        QCOMPARE(store.value({"count"}).toInt(), 3);
        QVERIFY(store.inputs().isEmpty());
    }
    void readerAcknowledgment_data() {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("expected");
        QTest::newRow("changed") << QString("changed") << 3;
        QTest::newRow("equal") << QString("equal") << 4;
        QTest::newRow("no-edit") << QString("no-edit") << 4;
        QTest::newRow("preview-base") << QString("preview") << 3;
        QTest::newRow("view-mode") << QString("view") << 3;
        QTest::newRow("preview-view-mode") << QString("preview-view") << 3;
        QTest::newRow("read-failure-recovery") << QString("read-failure") << 3;
        QTest::newRow("base-read-failure-recovery") << QString("base-read-failure") << 3;
        QTest::newRow("newer-input") << QString("newer-input") << 3;
        QTest::newRow("editor-closure") << QString("editor-closure") << 3;
    }
    void readerAcknowledgment() {
        QFETCH(QString, scenario);
        QFETCH(int, expected);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        compat::put(directory.filePath("base.json"), "{\"count\":1}");
        compat::put(directory.filePath("owner.json"),
                    "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"count\":4}}]}");
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(GENERIC_FIXTURE));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"directory", directory.path()}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QVERIFY(root->setProperty("previewBase", scenario.startsWith("preview")));
        QVERIFY(root->setProperty("runtimeMode", scenario.endsWith("view") ? "view" : "live"));
        QTRY_VERIFY(root->property("ready").toBool());
        QSignalSpy setup(root.get(), SIGNAL(setupFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setup"));
        QTRY_COMPARE(setup.size(), 1);
        QCOMPARE(root->property("failure").toString(), QString());
        bool rejected = false;
        QVERIFY(
            QMetaObject::invokeMethod(root.get(), "rejectsForged", Q_RETURN_ARG(bool, rejected)));
        QVERIFY(rejected);
        auto *owner = root->property("owner").value<ConfigQmlStore *>();
        auto *runtimeObject = engine.toScriptValue(root->property("runtime")).toQObject();
        QVERIFY(owner && runtimeObject);
        auto *reader = ConfigStoreTestAccess::overlay(
            *owner->core(), runtimeObject->property("editorId").toString());
        QVERIFY(reader);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReader(*owner->core(), reader->editorId(), &scheduler, held,
                                          onHeld);
        QSignalSpy saved(root.get(), SIGNAL(writerFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "apply", Q_ARG(int, expected),
                                          Q_ARG(bool, scenario != "no-edit")));
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(root->property("failure").toString(), QString());
        QCOMPARE(reader->value({"count"}).toInt(), 4);
        QCOMPARE(root->property("boundCount").toInt(), 4);
        const bool acquisitionFailure = scenario.endsWith("read-failure");
        const auto failedSource =
            directory.filePath(scenario.startsWith("base-") ? "base.json" : "owner.json");
        const auto goodBytes = compat::bytes(failedSource);
        if (acquisitionFailure)
            compat::put(failedSource, "{");
        QSignalSpy completed(root.get(), SIGNAL(readFinished()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "synchronize", Q_ARG(bool, true)));
        if (acquisitionFailure) {
            QTRY_COMPARE(completed.size(), 1);
            QCOMPARE(root->property("readStatus").toString(), QString("failed"));
            QCOMPARE(reader->value({"count"}).toInt(), 4);
            QCOMPARE(root->property("boundCount").toInt(), 4);
            compat::put(failedSource, goodBytes);
            QVERIFY(QMetaObject::invokeMethod(root.get(), "synchronize", Q_ARG(bool, false)));
        }
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        onHeld = [&] {
            for (const auto &packet : held)
                for (const auto &p : packet.overlays)
                    if (p.read.requestId)
                        loop.quit();
        };
        deadline.start(10000);
        loop.exec();
        onHeld = {};
        bool exact = false;
        for (const auto &packet : held)
            for (const auto &p : packet.overlays)
                exact |= p.read.requestId != 0;
        QVERIFY(exact);
        QCOMPARE(completed.size(), acquisitionFailure ? 1 : 0);
        if (scenario == "newer-input") {
            QVERIFY(QMetaObject::invokeMethod(root.get(), "newerInput"));
            QCOMPARE(root->property("boundCount").toInt(), 7);
        }
        if (scenario == "editor-closure")
            QVERIFY(QMetaObject::invokeMethod(root.get(), "closeEditor"));
        ConfigStoreTestAccess::releaseReader(*owner->core(), held);
        QCOMPARE(completed.size(), acquisitionFailure ? 2 : 1);
        QCOMPARE(root->property("readStatus").toString(), QString("observed"));
        QVERIFY(root->property("signalBeforeCallback").toBool());
        QCOMPARE(root->property("failure").toString(), QString());
        const auto displayed = scenario == "newer-input" ? 7 : expected;
        QCOMPARE(reader->value({"count"}).toInt(), displayed);
        QCOMPARE(root->property("callbackGetter").toInt(), displayed);
        QCOMPARE(root->property("callbackBinding").toInt(), displayed);
        QCOMPARE(root->property("pendingCount").toInt(), scenario == "newer-input" ? 1 : 0);
        QCOMPARE(root->property("captured").toInt(), expected);
        QProcess consumer;
        consumer.start(QCoreApplication::applicationFilePath(),
                       {"--inert-daemon", QString::number(root->property("captured").toInt())});
        QVERIFY(consumer.waitForFinished(10000));
        QCOMPARE(consumer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(consumer.exitCode(), 0);
        QCOMPARE(consumer.readAllStandardOutput().trimmed(),
                 "DAEMON_ARGV " + QByteArray::number(expected));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--inert-daemon") {
        if (app.arguments().size() != 3)
            return 2;
        std::printf("DAEMON_ARGV %s\n", qPrintable(app.arguments()[2]));
        return 0;
    }
    QmlGenericPrimitivesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_qml_generic_primitives.moc"
