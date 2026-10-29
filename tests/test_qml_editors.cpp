// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "storeprobe.h"
#include "confignumber.h"
#include "modelbudgets.h"
#include <QTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QDir>
#include <QUrl>
#include <cmath>
using namespace configqml;

static void put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("fixture write");
}
static QByteArray bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("fixture read");
    return file.readAll();
}
struct Fixture {
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    ConfigQmlStore *store = nullptr;
    Fixture(const QString &directory) {
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_EDITOR_FIXTURE));
        root.reset(component.createWithInitialProperties({{"directory", directory}}));
        if (!root)
            qFatal("%s", qPrintable(component.errorString()));
        store = qobject_cast<ConfigQmlStore *>(root->property("store").value<QObject *>());
    }
    void call(const char *name) { QVERIFY(QMetaObject::invokeMethod(root.get(), name)); }
    bool boolean(const char *name) {
        bool result = false;
        if (!QMetaObject::invokeMethod(root.get(), name, Q_RETURN_ARG(bool, result)))
            qFatal("invoke");
        return result;
    }
    int number(const char *name) const { return root->property(name).toInt(); }
    QVariantMap receipt() const { return root->property("receipt").toMap(); }
    ConfigQmlStore *editor() const {
        return qobject_cast<ConfigQmlStore *>(root->property("editor").value<QObject *>());
    }
};
class QmlEditorsTest : public QObject {
    Q_OBJECT
private slots:
    void draftConsumerAndRestart() {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QTRY_VERIFY(f.store->ready());
        QVERIFY(f.boolean("edit"));
        QCOMPARE(f.number("preview"), 7);
        QCOMPARE(f.number("observed"), 0);
        f.call("apply");
        QTRY_COMPARE(f.number("calls"), 1);
        QCOMPARE(f.receipt().value("status").toString(), QString("saved"));
        QCOMPARE(f.number("observed"), 7);
        QCOMPARE(f.number("other"), 8);
        QVERIFY(!f.editor()->dirty());
        Fixture reopened(dir.path());
        QTRY_VERIFY2(reopened.store->ready(), qPrintable(reopened.store->error()));
        QCOMPARE(reopened.number("observed"), 7);
        QCOMPARE(reopened.number("other"), 8);
        QVERIFY(f.boolean("edit"));
        f.call("change");
        f.call("save");
        QTRY_COMPARE(f.number("calls"), 2);
        QVERIFY(f.editor()->conflicted());
        QCOMPARE(f.number("preview"), 7);
        f.call("apply");
        QTRY_COMPARE(f.number("calls"), 3);
        QCOMPARE(f.receipt().value("status").toString(), QString("failed"));
        QVERIFY(f.editor()->dirty());
        QVERIFY(f.editor()->discard());
        QCOMPARE(f.number("preview"), 17);
    }
    void scopedConsumerAndProfile() {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QTRY_VERIFY(f.store->ready());
        QVERIFY(f.boolean("scopes"));
        f.call("save");
        QTRY_COMPARE(f.number("calls"), 1);
        QCOMPARE(f.number("observed"), 27);
        QCOMPARE(f.number("other"), 28);
        f.call("exportProfile");
        QTRY_COMPARE(f.number("calls"), 2);
        QCOMPARE(f.receipt().value("status").toString(), QString("exported"));
        const auto profile = bytes(dir.filePath("profile.json"));
        QVERIFY(!profile.contains("synthetic"));
        QVERIFY(!profile.contains("keyring:"));
        QVERIFY(!profile.contains("device-default"));
        f.call("change");
        f.call("save");
        QTRY_COMPARE(f.number("calls"), 3);
        f.call("importProfile");
        QTRY_COMPARE(f.number("calls"), 4);
        QCOMPARE(f.receipt().value("status").toString(), QString("saved"));
        QCOMPARE(f.number("observed"), 27);
        QCOMPARE(f.number("other"), 28);
        Fixture reopened(dir.path());
        QTRY_VERIFY2(reopened.store->ready(), qPrintable(reopened.store->error()));
        QCOMPARE(reopened.number("observed"), 27);
        QCOMPARE(reopened.number("other"), 28);
    }
    void workerJsMigrationAndTolerantAdmission() {
        QTemporaryDir dir;
        const QByteArray original =
            "{\"old\":6,\"unknown\":18446744073709551615,\"tolerant\":\"synthetic-invalid\"}";
        put(dir.filePath("a.json"), original);
        Fixture f(dir.path());
        QTRY_VERIFY2(f.store->ready(), qPrintable(f.store->error()));
        QCOMPARE(f.number("observed"), 6);
        QCOMPARE(f.store->core()->value({"a", "tolerant"}).toInt(), 11);
        QCOMPARE(bytes(dir.filePath("a.json.configqml-v0.bak")), original);
        const auto raw = ConfigJsonCodec().decode(bytes(dir.filePath("a.json")), "a").raw;
        QCOMPARE(raw.value("unknown").value<ConfigNumber>().token(),
                 QString("18446744073709551615"));
        QCOMPARE(raw.value("_configqmlVersion").value<ConfigNumber>().token(), QString("1"));
        Fixture reopened(dir.path());
        QTRY_VERIFY(reopened.store->ready());
        QCOMPARE(reopened.number("observed"), 6);
    }
    void thousandRowApplyBudget() {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QTRY_VERIFY(f.store->ready());
        QElapsedTimer timer;
        timer.start();
        f.call("budget");
        const auto ingress = timer.nsecsElapsed();
        QTRY_COMPARE(f.number("calls"), 1);
        QCOMPARE(f.receipt().value("status").toString(), QString("saved"));
        QCOMPARE(f.number("rows"), 1000);
        QCOMPARE(f.store->core()->listModel({"a", "rows"})->rowCount(), 1000);
        QCOMPARE(
            f.store->core()->value({"a", "rows"}).toList().last().toMap().value("title").toString(),
            QString("Title 999"));
        const auto publication = ConfigStoreTestAccess::publicationLibraryNanos(*f.store->core());
        qInfo("draft 1000 QML preparation+ingress=%lldus publication=%lldus", ingress / 1000,
              publication / 1000);
        if (enforceModelBudgets)
            QVERIFY(ingress + publication < 16000000);
        Fixture reopened(dir.path());
        QTRY_VERIFY(reopened.store->ready());
        QCOMPARE(reopened.number("rows"), 1000);
    }
    void failedDraftAndPartialProfileReceipts() {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QTRY_VERIFY(f.store->ready());
        QVERIFY(f.boolean("edit"));
        QVERIFY(QDir(dir.path()).mkdir("b.json"));
        f.call("apply");
        QTRY_COMPARE(f.number("calls"), 1);
        QCOMPARE(f.receipt().value("status").toString(), QString("failed"));
        QVERIFY(f.editor()->dirty());
        QCOMPARE(f.number("preview"), 7);
        QVERIFY(QDir(dir.path()).rmdir("b.json"));
        QVERIFY(f.editor()->discard());
        QVERIFY(f.boolean("edit"));
        f.call("apply");
        QTRY_COMPARE(f.number("calls"), 2);
        QCOMPARE(f.receipt().value("status").toString(), QString("saved"));
        f.call("exportProfile");
        QTRY_COMPARE(f.number("calls"), 3);
        QVERIFY(QFile::remove(dir.filePath("b.json")));
        QVERIFY(QDir(dir.path()).mkdir("b.json"));
        f.call("importProfile");
        QTRY_COMPARE(f.number("calls"), 4);
        QCOMPARE(f.receipt().value("status").toString(), QString("failed"));
        QCOMPARE(f.receipt().value("publicationState").toString(), QString("partial"));
        QCOMPARE(ConfigJsonCodec()
                     .decode(bytes(dir.filePath("a.json")), "a")
                     .raw.value("n")
                     .value<ConfigNumber>()
                     .token(),
                 QString("7"));
    }
    void detachInInputGetterCannotEscapeToStore() {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QTRY_VERIFY(f.store->ready());
        QVERIFY(f.boolean("detachDuringCapture"));
        QCOMPARE(f.store->core()->inputs().size(), 0);
    }
    void strictSourceAndJsFailureKeepOriginals_data() {
        QTest::addColumn<bool>("scriptFailure");
        QTest::newRow("strict") << false;
        QTest::newRow("js-no-qml-context") << true;
    }
    void strictSourceAndJsFailureKeepOriginals() {
        QFETCH(bool, scriptFailure);
        QTemporaryDir dir;
        const QByteArray original = "{\"n\":\"synthetic-private\"}";
        put(dir.filePath("config.json"), original);
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        const QByteArray migration = scriptFailure
                                         ? "version: 1; migrations: [{fromVersion: 0, transform: "
                                           "'function(raw) { raw.n = app.secret; return raw; }'}];"
                                         : "";
        component.setData(
            "import QtQml\nimport ConfigQml 1.0\nConfigStore { required property string file; "
            "autoSave: false;"
            "schema: ConfigSchema { " +
                migration +
                "ConfigValue { key: 'n'; valueType: ConfigValue.Integer; defaultValue: 3; "
                "sensitive: true; admissionPolicy: ConfigValue.Strict } }"
                "sources: [JsonFileSource { path: file }] }",
            QUrl("file:///configqml-test.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"file", dir.filePath("config.json")}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = qobject_cast<ConfigQmlStore *>(root.get());
        QVERIFY(store);
        QTRY_COMPARE(store->status(), ConfigQmlStore::Error);
        QVERIFY(!store->error().contains("synthetic-private"));
        QCOMPARE(bytes(dir.filePath("config.json")), original);
        QCOMPARE(store->error(),
                 scriptFailure ? QString("migration-transform") : QString("strict-file-admission"));
    }
    void plainMigrationPreservesNativeAtoms() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        component.setData(
            "import QtQml\nimport ConfigQml 1.0\nConfigStore { autoSave: false;"
            "schema: ConfigSchema { version: 1;"
            "migrations: [{fromVersion: 0, transform: "
            "'function(raw) { raw.extra = -0; return raw; }'}];"
            "ConfigValue { key: 'huge'; valueType: ConfigValue.UInt64; defaultValue: '0' }"
            "ConfigValue { key: 'url'; valueType: ConfigValue.Url;"
            "defaultValue: Qt.resolvedUrl('https://example.test/default') }"
            "ConfigValue { key: 'real'; valueType: ConfigValue.Real; defaultValue: 0 } }"
            "sources: [MemorySource { data: ({huge: '18446744073709551615',"
            "url: Qt.resolvedUrl('https://example.test/value'), real: -0}) }] }",
            QUrl("file:///configqml-native-atoms.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = qobject_cast<ConfigQmlStore *>(root.get());
        QVERIFY(store);
        QTRY_VERIFY2(store->ready(), qPrintable(store->error()));
        QCOMPARE(store->read({"huge"}).toString(), QString("18446744073709551615"));
        QCOMPARE(store->core()->value({"url"}).toUrl(), QUrl("https://example.test/value"));
        QVERIFY(std::signbit(store->core()->value({"real"}).toDouble()));
    }
};
QTEST_GUILESS_MAIN(QmlEditorsTest)
#include "test_qml_editors.moc"
