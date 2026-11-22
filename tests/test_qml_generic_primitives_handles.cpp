// SPDX-License-Identifier: LGPL-3.0-or-later
// Cold native-driven runs of the five addendum-02 QML sketches.
#include "configqmlstore.h"
#include "configqmlhandle.h"
#include "confighandle.h"
#include "compatfixture.h"
#include "storeprobe.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>

using namespace configqml;
namespace {
// Plain JSON view of the persisted bytes; numbers compare as JSON doubles.
QVariantMap decode(const QString &file) {
    return QJsonDocument::fromJson(compat::bytes(file)).object().toVariantMap();
}
bool waitFor(QObject *root, QSignalSpy &spy, const QString &name, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        for (const auto &arguments : std::as_const(spy))
            if (arguments.value(0).toString() == name)
                return true;
        if (timer.elapsed() > timeout)
            return false;
        spy.wait(100);
    }
    Q_UNUSED(root);
}
} // namespace
class QmlGenericHandlesTest : public QObject {
    Q_OBJECT
private slots:
    void cachedSelectionBindings_data() {
        QTest::addColumn<bool>("rootFirst");
        QTest::addColumn<bool>("unionLookup");
        QTest::newRow("scope-root-first") << true << false;
        QTest::newRow("scope-selected-first") << false << false;
        QTest::newRow("union-root-first") << true << true;
        QTest::newRow("union-selected-first") << false << true;
    }
    void cachedSelectionBindings() {
        QFETCH(bool, rootFirst);
        QFETCH(bool, unionLookup);
        using D = ConfigDescriptor;
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"general", D::group({{"count", D::leaf(D::Type::SafeInteger, 2)},
                                            {"other", D::leaf(D::Type::SafeInteger, 3)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"settings", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QQmlEngine engine;
        auto *handle = ConfigQmlHandle::owned(store.root(), &engine, &engine, "cache-test");
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQml
            QtObject {
                required property var config
                required property bool rootFirst
                required property bool unionLookup
                property var selected: null
                property var whole: null
                property var refusal: null
                property var accepted: null
                readonly property int bound: selected
                    ? (unionLookup ? selected.scopes.general.fields.count.value
                                   : selected.fields.count.value) : -1
                function setup() {
                    const narrow = config.select([["general", "count"]]);
                    function lookup(parent) {
                        return unionLookup ? parent.select([["general"]]) : parent.scope(["general"]);
                    }
                    if (rootFirst) { whole = lookup(config); selected = lookup(narrow); }
                    else { selected = lookup(narrow); whole = lookup(config); }
                    refusal = selected.set([{path: unionLookup ? ["general", "other"] : ["other"], value: 99}]);
                }
                function update() {
                    accepted = (unionLookup ? selected.scopes.general.fields.count
                                            : selected.fields.count).set(7);
                }
            }
        )QML",
                          QUrl("file:cache-identity.qml"));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"config", QVariant::fromValue(handle)},
                                                   {"rootFirst", rootFirst},
                                                   {"unionLookup", unionLookup}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setup"));
        auto *selected = root->property("selected").value<QObject *>();
        auto *whole = root->property("whole").value<QObject *>();
        QVERIFY(selected && whole && selected != whole);
        QTRY_COMPARE(root->property("bound").toInt(), 2);
        auto *refused = root->property("refusal").value<QObject *>();
        QVERIFY(refused);
        QTRY_VERIFY(refused->property("isFinished").toBool());
        QVERIFY(refused->property("isError").toBool());
        QCOMPARE(engine.toScriptValue(refused->property("error")).property("code").toString(),
                 QString("outside-selection"));
        QCOMPARE(store.value({"general", "other"}).toInt(), 3);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "update"));
        auto *accepted = root->property("accepted").value<QObject *>();
        QVERIFY(accepted);
        QTRY_VERIFY(accepted->property("isFinished").toBool());
        QVERIFY(!accepted->property("isError").toBool());
        QCOMPARE(root->property("bound").toInt(), 7);
        QCOMPARE(store.value({"general", "other"}).toInt(), 3);
    }
    void sketches() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        compat::put(directory.filePath("settings.json"), "{\"count\":1}");
        compat::put(directory.filePath("general.json"),
                    "{\"scaleRatio\":2,\"notes\":\"keep\",\"opaque\":true}");
        compat::put(directory.filePath("bar.json"), "{\"position\":\"top\"}");
        compat::put(directory.filePath("owner.json"),
                    "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"count\":4,"
                    "\"opaque\":\"o\"}}]}");
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(HANDLES_FIXTURE));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"directory", directory.path()}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY(root->property("ready").toBool());
        QSignalSpy finished(root.get(), SIGNAL(finished(QString)));
        auto call = [&](const char *method, const QString &name) {
            if (!QMetaObject::invokeMethod(root.get(), method))
                return false;
            return waitFor(root.get(), finished, name);
        };
        auto failure = [&] { return root->property("failure").toString(); };

        // Sketch 1: bound generated field; opening an editor stages nothing.
        QTRY_COMPARE(root->property("scale").toDouble(), 2.0);
        QVERIFY(!root->property("labelPresent").toBool());
        const int notifications = root->property("scaleNotifications").toInt();
        QVERIFY(call("openEditor", "openEditor"));
        QCOMPARE(failure(), QString());
        QCOMPARE(root->property("editedScale").toDouble(), 2.0);
        const auto general = compat::bytes(directory.filePath("general.json"));

        // Sketch 2: cancellable configuration Reset; protected notes untouched.
        QVERIFY(call("resetConfiguration", "reset"));
        QCOMPARE(failure(), QString());
        QTRY_COMPARE(root->property("editedScale").toDouble(), 1.0);
        QCOMPARE(root->property("scale").toDouble(), 2.0);
        QCOMPARE(compat::bytes(directory.filePath("general.json")), general);
        QVERIFY(call("cancelReset", "cancel"));
        QCOMPARE(failure(), QString());
        QTRY_COMPARE(root->property("editedScale").toDouble(), 2.0);
        QCOMPARE(compat::bytes(directory.filePath("general.json")), general);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setScale", Q_ARG(double, 5.0)));
        QVERIFY(waitFor(root.get(), finished, "setScale"));
        QVERIFY(call("applyEdit", "applyEdit"));
        QCOMPARE(failure(), QString());
        QCOMPARE(root->property("scale").toDouble(), 5.0);
        QCOMPARE(root->property("scaleNotifications").toInt(), notifications + 1);
        QCOMPARE(root->property("labelNotifications").toInt(), 0);
        QCOMPARE(decode(directory.filePath("general.json")),
                 (QVariantMap{{"scaleRatio", 5.0}, {"notes", "keep"}, {"opaque", true}}));

        // Sketch 4: scoped save excludes the pending sibling in another file.
        const auto savedGeneral = compat::bytes(directory.filePath("general.json"));
        QVERIFY(call("saveBarOnly", "saveBar"));
        QCOMPARE(failure(), QString());
        QCOMPARE(decode(directory.filePath("bar.json")), (QVariantMap{{"position", "left"}}));
        QCOMPARE(compat::bytes(directory.filePath("general.json")), savedGeneral);
        QCOMPARE(root->property("scale").toDouble(), 9.0);

        QVERIFY(call("openOverlays", "overlays"));
        QCOMPARE(failure(), QString());
        QVERIFY(call("forgedToken", "forged"));
        QVERIFY(call("readOnlyRefusal", "readOnly"));
        QCOMPARE(failure(), QString());

        // Sketch 5: sparse override; unset displays inheritance, Apply removes
        // only this placement's key and keeps the opaque sibling.
        QTRY_COMPARE(root->property("runtimeCount").toInt(), 4);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "choose", Q_ARG(int, 6)));
        QVERIFY(waitFor(root.get(), finished, "choose"));
        QTRY_COMPARE(root->property("displayed").toInt(), 6);
        QVERIFY(!root->property("inherited").toBool());
        QVERIFY(call("inheritAgain", "inherit"));
        QCOMPARE(failure(), QString());
        QTRY_VERIFY(root->property("inherited").toBool());
        QTRY_COMPARE(root->property("displayed").toInt(), 1);
        QVERIFY(call("applyInstance", "instance"));
        QCOMPARE(failure(), QString());
        QCOMPARE(root->property("instanceDisplayed").toInt(), 1);
        QVERIFY(root->property("instanceInherited").toBool());
        QCOMPARE(root->property("runtimeCount").toInt(), 1);
        QCOMPARE(decode(directory.filePath("owner.json")),
                 (QVariantMap{{"rows", QVariantList{QVariantMap{
                                           {"id", "x"},
                                           {"instanceSettings", QVariantMap{{"opaque", "o"}}}}}}}));
        // Sketch 3: the composite Apply stays unfinished while the runtime
        // reader's capture is held; bindings are current before finished. The
        // hold reroutes this store's packets for the rest of the test, so it
        // runs last.
        auto *owner = root->property("owner").value<ConfigQmlStore *>();
        auto *runtimeObject = engine.toScriptValue(root->property("runtime")).toQObject();
        QVERIFY(owner && runtimeObject);
        QTRY_COMPARE(root->property("runtimeCount").toInt(), 1);
        auto *reader = ConfigStoreTestAccess::overlay(
            *owner->core(), runtimeObject->property("editorId").toString());
        QVERIFY(reader);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReader(*owner->core(), reader->editorId(), &scheduler, held,
                                          onHeld);
        QEventLoop loop;
        onHeld = [&] {
            for (const auto &packet : held)
                for (const auto &p : packet.overlays)
                    if (p.read.requestId)
                        loop.quit();
        };
        QVERIFY(QMetaObject::invokeMethod(root.get(), "applyAutoTile", Q_ARG(int, 3)));
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        onHeld = {};
        bool exact = false;
        for (const auto &packet : held)
            for (const auto &p : packet.overlays)
                exact |= p.read.requestId != 0;
        QVERIFY(exact);
        QVERIFY(!waitFor(root.get(), finished, "autoTile", 200));
        auto *reply = engine.toScriptValue(root->property("autoTileReply")).toQObject();
        QVERIFY(reply);
        QVERIFY(!reply->property("isFinished").toBool());
        QVERIFY(!root->property("finishedEarly").toBool());
        QCOMPARE(root->property("runtimeCount").toInt(), 1);
        ConfigStoreTestAccess::releaseReader(*owner->core(), held);
        QVERIFY(waitFor(root.get(), finished, "autoTile"));
        QCOMPARE(failure(), QString());
        QCOMPARE(root->property("boundAtFinish").toInt(), 3);
        QCOMPARE(root->property("captured").toInt(), 3);
        QProcess consumer;
        consumer.start(QCoreApplication::applicationFilePath(),
                       {"--inert-daemon", QString::number(root->property("captured").toInt())});
        QVERIFY(consumer.waitForFinished(10000));
        QCOMPARE(consumer.exitCode(), 0);
        QCOMPARE(consumer.readAllStandardOutput().trimmed(), QByteArray("DAEMON_ARGV 3"));

        QCOMPARE(decode(directory.filePath("settings.json")), (QVariantMap{{"count", 1.0}}));
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
    QmlGenericHandlesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_qml_generic_primitives_handles.moc"
