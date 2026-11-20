// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsoncodec.h"
#include <QTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QFile>
using namespace configqml;
namespace {
void writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        qFatal("fixture write");
}
QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        qFatal("fixture read");
    return f.readAll();
}
} // namespace
class QmlDraftResetTest : public QObject {
    Q_OBJECT
private slots:
    void shapesAndPresence() {
        QTemporaryDir dir;
        const auto file = dir.filePath("shapes-user.json");
        writeFile(
            file,
            "{\"flag\":true,\"zero\":9,\"text\":\"chosen\",\"list\":[\"chosen\"],"
            "\"record\":{\"label\":\"chosen\",\"opaque\":true},\"emptyRecord\":{\"opaque\":true},"
            "\"map\":{\"chosen\":\"chosen\"},\"nullable\":\"chosen\",\"optional\":\"chosen\","
            "\"outsideUnknown\":false}");
        writeFile(dir.filePath("shapes-legacy.json"),
                  "{\"flag\":false,\"zero\":0,\"text\":\"\",\"list\":[],"
                  "\"record\":{\"label\":\"lower\",\"wide\":18446744073709551615,\"__proto__\":{"
                  "\"x\":false},\"constructor\":\"own\"},"
                  "\"emptyRecord\":{},\"map\":{},\"nullable\":null}");
        for (bool cold : {false, true}) {
            QQmlEngine engine;
            engine.addImportPath(QML_IMPORT_ROOT);
            QQmlComponent component(&engine, QUrl::fromLocalFile(DRAFT_RESET_SHAPES));
            std::unique_ptr<QObject> root(
                component.createWithInitialProperties({{"directory", dir.path()}}));
            QVERIFY2(root, qPrintable(component.errorString()));
            QTRY_VERIFY_WITH_TIMEOUT(root->property("ready").toBool(), 10000);
            QVERIFY(QMetaObject::invokeMethod(root.get(), cold ? "runRestart" : "run"));
            QTRY_VERIFY_WITH_TIMEOUT(root->property("finished").toBool() ||
                                         !root->property("failure").toString().isEmpty(),
                                     10000);
            QCOMPARE(root->property("failure").toString(), QString());
            QVERIFY(root->property("finished").toBool());
            QCOMPARE(ConfigJsonCodec().decode(readFile(file), "user").raw,
                     QVariantMap({{"outsideUnknown", false}}));
        }
    }
    void consumer() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        const QByteArray original = "{\"n\":9}";
        writeFile(file, original);
        writeFile(dir.filePath("legacy-reset.json"), "{\"n\":7,\"other\":8}");
        for (bool cold : {false, true}) {
            QQmlEngine engine;
            engine.addImportPath(QML_IMPORT_ROOT);
            QQmlComponent component(&engine, QUrl::fromLocalFile(DRAFT_RESET_FIXTURE));
            std::unique_ptr<QObject> root(
                component.createWithInitialProperties({{"directory", dir.path()}}));
            QVERIFY2(root, qPrintable(component.errorString()));
            QTRY_VERIFY_WITH_TIMEOUT(root->property("ready").toBool(), 10000);
            QVERIFY(QMetaObject::invokeMethod(root.get(), cold ? "runRestart" : "run"));
            if (!cold) {
                QTRY_VERIFY(root->property("action").toString() == "check-before-apply" ||
                            !root->property("failure").toString().isEmpty());
                QCOMPARE(root->property("failure").toString(), QString());
                QCOMPARE(readFile(file), original);
                QVERIFY(QMetaObject::invokeMethod(root.get(), "checkedDisk"));
                QTRY_VERIFY(root->property("action").toString() == "publish-foreign");
                writeFile(dir.filePath("legacy-reset.json"), "{\"n\":17,\"other\":18}");
                QVERIFY(QMetaObject::invokeMethod(root.get(), "reloadForeign"));
                QTRY_VERIFY(root->property("action").toString() == "restore-foreign" ||
                            !root->property("failure").toString().isEmpty());
                QCOMPARE(root->property("failure").toString(), QString());
                QCOMPARE(readFile(file), original);
                writeFile(dir.filePath("legacy-reset.json"), "{\"n\":7,\"other\":8}");
                QVERIFY(QMetaObject::invokeMethod(root.get(), "reloadRestored"));
            }
            QTRY_VERIFY_WITH_TIMEOUT(root->property("finished").toBool() ||
                                         !root->property("failure").toString().isEmpty(),
                                     10000);
            QCOMPARE(root->property("failure").toString(), QString());
            QVERIFY(root->property("finished").toBool());
            const auto raw = ConfigJsonCodec().decode(readFile(file), "user");
            QVERIFY(raw.ok());
            QVERIFY(!raw.raw.contains("n"));
            QVERIFY(!raw.raw.contains("other"));
        }
    }
};
QTEST_GUILESS_MAIN(QmlDraftResetTest)
#include "test_qml_draft_reset.moc"
