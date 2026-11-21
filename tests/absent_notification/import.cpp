// SPDX-License-Identifier: LGPL-3.0-or-later
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// Deliberately no configqml headers or link: the installed plugin must provide
// registration and implementation, just as it does for an import-only app.
class ImportAbsentTest : public QObject {
    Q_OBJECT
private slots:
    void retainedBindings_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"reset", "apply", "draft", "preview", "overlay-editor",
                                 "overlay-live", "external", "inherited-view", "inherited-live"})
            QTest::newRow(mode) << QString(mode);
    }
    void retainedBindings() {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        const auto put = [&](const QString &name, const QByteArray &bytes) {
            QSaveFile file(directory.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
                   file.commit();
        };
        const bool ownOverride = mode.startsWith("overlay-");
        QVERIFY(put("user.json", ownOverride ? "{}" : "{\"flag\":true}"));
        QVERIFY(put("owner.json",
                    ownOverride ? "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"flag\":true}}]}"
                                : "{\"rows\":[{\"id\":\"x\"}]}"));
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(ABSENT_FIXTURE));
        // ConfigValue.Boolean's public enum value is zero.
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"directory", directory.path()}, {"kind", 0}, {"initial", true}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY(root->property("ready").toBool());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "setup", Q_ARG(QString, mode)));
        QTRY_VERIFY(root->property("done").toBool());
        QCOMPARE(root->property("failure").toString(), QString());
        auto *subject = engine.toScriptValue(root->property("subject")).toQObject();
        QVERIFY(subject);
        const auto index = subject->metaObject()->indexOfSignal("flagChanged()");
        QVERIFY(index >= 0);
        QSignalSpy notifications(subject, subject->metaObject()->method(index));
        QVERIFY(notifications.isValid());
        for (int cycle = 0; cycle < 2; ++cycle) {
            QCOMPARE(root->property("observed").toBool(), true);
            const auto before = root->property("changes").toInt();
            const auto signalsBefore = notifications.size();
            if (mode == "external")
                QVERIFY(put("user.json", "{}"));
            else {
                QVERIFY(QMetaObject::invokeMethod(root.get(), "removeValue"));
                QTRY_VERIFY(root->property("done").toBool());
            }
            QTRY_VERIFY(!subject->property("flag").isValid());
            QVERIFY(engine.toScriptValue(root->property("observed")).isUndefined());
            QCOMPARE(root->property("changes").toInt(), before + 1);
            QCOMPARE(notifications.size(), signalsBefore + 1);
            if (mode == "external")
                QVERIFY(put("user.json", "{\"flag\":true}"));
            else {
                QVERIFY(QMetaObject::invokeMethod(root.get(), "restoreValue"));
                QTRY_VERIFY(root->property("done").toBool());
            }
            QTRY_VERIFY(subject->property("flag").toBool());
            QCOMPARE(root->property("observed").toBool(), true);
            QCOMPARE(root->property("changes").toInt(), before + 2);
            QCOMPARE(notifications.size(), signalsBefore + 2);
            QCOMPARE(root->property("failure").toString(), QString());
        }
    }
};
QTEST_GUILESS_MAIN(ImportAbsentTest)
#include "import.moc"
