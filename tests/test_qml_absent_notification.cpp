// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configjsoncodec.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace configqml;
namespace {
void put(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        qFatal("fixture write");
}
} // namespace
class AbsentNotificationTest : public QObject {
    Q_OBJECT
private slots:
    void defaultsAndMasks() {
        QTemporaryDir directory;
        put(directory.filePath("user.json"), "{\"flag\":true,\"defaulted\":true,\"masked\":false}");
        put(directory.filePath("owner.json"), "{}");
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(ABSENT_FIXTURE));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"directory", directory.path()},
                                                   {"kind", ConfigQmlValue::Boolean},
                                                   {"initial", true}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY(root->property("ready").toBool());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setup", Q_ARG(QString, QString("reset"))));
        auto *core = root->property("shared").value<ConfigQmlStore *>()->core();
        QCOMPARE(root->property("defaultObserved").toBool(), true);
        QCOMPARE(root->property("maskedObserved").toBool(), true);
        const auto defaultsBefore = root->property("defaultChanges").toInt();
        const auto masksBefore = root->property("maskedChanges").toInt();
        bool settled = false;
        const auto done = [&](const ConfigResult &) { settled = true; };
        QVERIFY(core->reset({"settings"}, "user", root.get(), done).accepted);
        QTRY_VERIFY(settled);
        QCOMPARE(root->property("defaultObserved").toBool(), false);
        QCOMPARE(root->property("defaultChanges").toInt(), defaultsBefore + 1);
        QCOMPARE(root->property("maskedChanges").toInt(), masksBefore);
        settled = false;
        QVERIFY(core->clearRuntime({"settings", "masked"}, root.get(), done).accepted);
        QTRY_VERIFY(settled);
        QVERIFY(engine.toScriptValue(root->property("maskedObserved")).isUndefined());
        QCOMPARE(root->property("maskedChanges").toInt(), masksBefore + 1);
        settled = false;
        QVERIFY(core->setRuntime({"settings", "masked"}, true, root.get(), done).accepted);
        QTRY_VERIFY(settled);
        QCOMPARE(root->property("maskedObserved").toBool(), true);
        QCOMPARE(root->property("maskedChanges").toInt(), masksBefore + 2);
        settled = false;
        QVERIFY(core->setUser({"settings", "masked"}, false, root.get(), done).accepted);
        QTRY_VERIFY(settled);
        QCOMPARE(root->property("maskedChanges").toInt(), masksBefore + 2);
        settled = false;
        QVERIFY(core->clearRuntime({"settings", "masked"}, root.get(), done).accepted);
        QTRY_VERIFY(settled);
        QCOMPARE(root->property("maskedObserved").toBool(), false);
        QCOMPARE(root->property("maskedChanges").toInt(), masksBefore + 3);
    }
    void transitions_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QVariant>("initial");
        for (const auto *mode : {"early", "reset", "apply", "draft", "preview", "overlay-editor",
                                 "overlay-live", "external", "inherited-view", "inherited-live"}) {
            const QList<QPair<int, QVariant>> values{
                {ConfigQmlValue::Boolean, true},
                {ConfigQmlValue::Boolean, false},
                {ConfigQmlValue::Boolean, QVariant::fromValue(nullptr)},
                {ConfigQmlValue::Integer, 0},
                {ConfigQmlValue::Integer, 42},
                {ConfigQmlValue::String, QString()},
                {ConfigQmlValue::String, QString("chosen")},
                {ConfigQmlValue::StringList, QVariantList{}},
                {ConfigQmlValue::StringList, QVariantList{QString("chosen")}},
                {ConfigQmlValue::Record, QVariantMap{}},
                {ConfigQmlValue::Record, QVariantMap{{"opaque", false}}}};
            for (int i = 0; i < values.size(); ++i)
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(i)))
                    << QString(mode) << values[i].first << values[i].second;
        }
    }
    void transitions() {
        QFETCH(QString, mode);
        QFETCH(int, kind);
        QFETCH(QVariant, initial);
        QTemporaryDir directory;
        const auto file = directory.filePath("user.json");
        const auto encoded = ConfigJsonCodec().encode({{"flag", initial}}, "user").bytes;
        put(file, mode.startsWith("overlay-") ? QByteArray("{}") : encoded);
        const auto overrides =
            mode.startsWith("overlay-") ? QVariantMap{{"flag", initial}} : QVariantMap{};
        put(directory.filePath("owner.json"),
            ConfigJsonCodec()
                .encode({{"rows",
                          QVariantList{QVariantMap{{"id", "x"}, {"instanceSettings", overrides}}}}},
                        "owner")
                .bytes);
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(ABSENT_FIXTURE));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"directory", directory.path()}, {"kind", kind}, {"initial", initial}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY(root->property("ready").toBool());
        auto *shared = root->property("shared").value<ConfigQmlStore *>();
        QVERIFY(shared);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setup", Q_ARG(QString, mode)));
        QTRY_VERIFY(root->property("done").toBool());
        QCOMPARE(root->property("failure").toString(), QString());
        QCOMPARE(engine.toScriptValue(root->property("observed")).toVariant(), initial);
        auto *subject = engine.toScriptValue(root->property("subject")).toQObject();
        QVERIFY(subject);
        const auto signal = subject->metaObject()->indexOfSignal("flagChanged()");
        QVERIFY(signal >= 0);
        QSignalSpy notifications(subject, subject->metaObject()->method(signal));
        QVERIFY(notifications.isValid());
        for (int cycle = 0; cycle < 2; ++cycle) {
            const int before = root->property("changes").toInt();
            const auto signalsBefore = notifications.size();
            if (mode == "external")
                put(file, "{}");
            else {
                QVERIFY(QMetaObject::invokeMethod(root.get(), "removeValue"));
                QTRY_VERIFY(root->property("done").toBool());
            }
            QCOMPARE(root->property("failure").toString(), QString());
            if (mode == "external" || mode == "reset" || mode == "early" || mode == "apply")
                QTRY_VERIFY(!shared->core()->value({"settings", "flag"}).isValid());
            QTRY_VERIFY(!subject->property("flag").isValid());
            QVERIFY2(engine.toScriptValue(root->property("observed")).isUndefined(),
                     "retained bound QML reader did not observe absence");
            QCOMPARE(root->property("changes").toInt(), before + 1);
            QCOMPARE(notifications.size(), signalsBefore + 1);
            // A repeated removal or provenance-only file observation is not a
            // second effective-value transition, even for preview subscribers.
            if (mode == "external") {
                put(file, "{}");
                bool reloaded = false;
                shared->core()->reload(root.get(), [&](const ConfigResult &) { reloaded = true; });
                QTRY_VERIFY(reloaded);
            } else {
                QVERIFY(QMetaObject::invokeMethod(root.get(), "removeValue"));
                QTRY_VERIFY(root->property("done").toBool());
            }
            QCOMPARE(root->property("failure").toString(), QString());
            QCOMPARE(root->property("changes").toInt(), before + 1);
            QCOMPARE(notifications.size(), signalsBefore + 1);
            if (mode == "external")
                put(file, encoded);
            else {
                QVERIFY(QMetaObject::invokeMethod(root.get(), "restoreValue"));
                QTRY_VERIFY(root->property("done").toBool());
            }
            if (mode == "external")
                QTRY_VERIFY(shared->core()->value({"settings", "flag"}).isValid());
            QTRY_COMPARE(engine.toScriptValue(subject->property("flag")).toVariant(), initial);
            QCOMPARE(engine.toScriptValue(root->property("observed")).toVariant(), initial);
            QCOMPARE(root->property("changes").toInt(), before + 2);
            QCOMPARE(notifications.size(), signalsBefore + 2);
            QCOMPARE(root->property("failure").toString(), QString());
        }
    }
};
QTEST_GUILESS_MAIN(AbsentNotificationTest)
#include "test_qml_absent_notification.moc"
