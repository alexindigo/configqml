// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configqmlstore.h"
#include "configqmlvalue.h"
#include "confignumber.h"
#include "storeprobe.h"
#include "configenvironment.h"
#include "configscope.h"
#include "configstorehandle.h"
#include "configfacade.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
#include <QEventLoop>
#include <QTimer>
#include <QMutexLocker>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;

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
static ConfigStoreOptions options(const QString &file = {}) {
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 3)}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"raw", {}, {}}},
                 true}};
    if (!file.isEmpty())
        o.jsonFiles = {{"user", "raw", file}};
    return o;
}
static ConfigResult request(ConfigStore &store, const std::function<quint64()> &dispatch) {
    ConfigResult result;
    QEventLoop loop;
    quint64 id = 0;
    QObject::connect(&store, &ConfigStore::completed, &loop, [&](const ConfigResult &r) {
        if (r.requestId == id) {
            result = r;
            loop.quit();
        }
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    id = dispatch();
    loop.exec();
    if (!result.requestId)
        qFatal("request timeout");
    return result;
}
static QString diagnosticText(const QList<ConfigDiagnostic> &diagnostics) {
    QString result;
    for (const auto &d : diagnostics)
        result += d.path.join('/') + ':' + d.code + '\n';
    return result;
}
static QStringList messages;
static QMutex messageMutex;
static void captureMessage(QtMsgType, const QMessageLogContext &, const QString &text) {
    QMutexLocker lock(&messageMutex);
    messages.append(text);
}
class MessageCapture {
public:
    MessageCapture() {
        messages.clear();
        previous = qInstallMessageHandler(captureMessage);
    }
    ~MessageCapture() { qInstallMessageHandler(previous); }

private:
    QtMessageHandler previous;
};
class NativeFacade : public ConfigFacade {
public:
    using ConfigFacade::ConfigFacade;
    using ConfigFacade::readSetting;
    using ConfigFacade::writeSetting;
};

class M5Boundaries : public QObject {
    Q_OBJECT
private slots:
    void nullableFacadeAndDestroyedOwner() {
        NativeFacade detached(nullptr);
        QVERIFY(!detached.readSetting({"n"}).isValid());
        QSignalSpy rejected(&detached, &ConfigFacade::inputRejected);
        const auto write = detached.writeSetting({"n"}, 1);
        QVERIFY(!write.accepted);
        QCOMPARE(write.errors.first().code, QString("detached-or-undeclared"));
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected[0][1].toString(), QString("detached-or-undeclared"));

        auto o = options();
        auto secret = D::leaf(T::Map, QVariantMap{});
        secret.element = {D::leaf(T::SafeInteger, 0)};
        secret.sensitive = true;
        o.schema = ConfigSchema(D::group({{"credentials", secret}}));
        auto owner = std::make_unique<ConfigStore>(o);
        QTRY_COMPARE(owner->status(), ConfigStore::Status::Ready);
        NativeFacade retained(owner.get(), {"credentials"});
        QSignalSpy retainedRejected(&retained, &ConfigFacade::inputRejected);
        owner.reset();
        QVERIFY(!retained.readSetting({"synthetic-private-key"}).isValid());
        const auto failure = retained.writeSetting({"synthetic-private-key"}, 1);
        QVERIFY(!failure.accepted);
        QCOMPARE(failure.errors.first().path, ConfigPath({"credentials", "<redacted>"}));
        QCOMPARE(retainedRejected.size(), 1);
        QCOMPARE(retainedRejected[0][0].toStringList(), QStringList{"<redacted>"});
    }
    void diagnosticCanary() {
        MessageCapture logCapture;
        const QString canary = QString("M5_") + "DIAGNOSTIC_CANARY_72f9";
        QStringList surfaces;
        auto credentials = D::leaf(T::Map, QVariantMap{});
        credentials.sensitive = true;
        credentials.admissionPolicy = D::AdmissionPolicy::Strict;
        credentials.element = {D::leaf(T::SafeInteger, 0)};
        QTemporaryDir dir;
        const auto file = dir.filePath("config.json");
        put(file, "{}");
        auto o = options(file);
        o.schema = ConfigSchema(D::group({{"credentials", credentials}}));
        const QVariantMap invalid{{canary, QString("invalid")}};
        surfaces.append(diagnosticText(credentials.validate(invalid, {"credentials"}).errors));
        surfaces.append(diagnosticText(o.schema.validate({"credentials"}, invalid).errors));
        ConfigLayers layers(o.schema, o.layers);
        auto source = o.layers.first().sources.first();
        source.raw = {{"credentials", invalid}};
        surfaces.append(diagnosticText(layers.replaceSource("user", source).errors));
        surfaces.append(diagnosticText(layers.diagnostics("user", "raw")));
        QObject unsupported;
        source.raw = {{"credentials", QVariantMap{{canary, QVariant::fromValue(&unsupported)}}}};
        surfaces.append(diagnosticText(layers.replaceSource("user", source).errors));
        const auto invalidBytes = encodeJsonValue(QVariantMap{{"credentials", invalid}}).bytes;
        surfaces.append(diagnosticText(
            mapEnvironment(o.schema, {{"CREDENTIALS", {"credentials"}}},
                           {{"CREDENTIALS", QString::fromUtf8(encodeJsonValue(invalid).bytes)}})
                .errors));
        const auto malformed =
            QByteArray("{\"credentials\":{") + encodeJsonValue(canary).bytes + ":";
        auto codecError = ConfigJsonCodec(o.schema.diagnostics()).decode(malformed, "raw").error;
        surfaces.append(codecError.path.join('/') + ':' + codecError.code);
        auto ownership = ownValue(source.raw, {}, o.schema.diagnostics());
        surfaces.append(diagnosticText(ownership.errors));
        auto encoding = ConfigJsonCodec(o.schema.diagnostics()).encode(source.raw, "raw");
        surfaces.append(encoding.error.path.join('/') + ':' + encoding.error.code);
        ConfigResult untrusted;
        untrusted.error = {"type", "raw", "admission", {}, {"credentials", canary}};
        untrusted.publications.append({"raw", false, {}, "known", untrusted.error});
        surfaces.append(
            QString::fromUtf8(encodeJsonValue(qmlResult(untrusted, o.schema.diagnostics())).bytes));
        auto shared = std::make_shared<ConfigLayers>(o.schema, o.layers);
        ConfigScope scope(shared, {}, false);
        surfaces.append(diagnosticText(scope.setUser({"credentials", canary}, 1).errors));
        scope.detach();
        surfaces.append(diagnosticText(scope.scope({}).setUser({"credentials", canary}, 1).errors));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy rejected(&store, &ConfigStore::inputRejected);
        auto *readOnly = store.scope({}, false);
        surfaces.append(diagnosticText(readOnly->setUser({"credentials", canary}, 1).errors));
        readOnly->detach();
        surfaces.append(
            diagnosticText(readOnly->scope({})->setUser({"credentials", canary}, 1).errors));
        NativeFacade facade(&store);
        QSignalSpy facadeRejected(&facade, &ConfigFacade::inputRejected);
        surfaces.append(diagnosticText(facade.writeSetting({"credentials", canary}, 1).errors));
        for (const auto &signal : facadeRejected)
            surfaces.append(signal[0].toStringList().join('/'));
        ConfigRequestHandle handle;
        auto result = request(store, [&] {
            handle = store.setUser({"credentials"}, invalid);
            return handle.requestId;
        });
        surfaces.append(diagnosticText(handle.errors));
        surfaces.append(QString::fromUtf8(encodeJsonValue(qmlResult(result)).bytes));
        request(store, [&] {
            handle = store.setUser({"credentials", canary}, 1);
            return handle.requestId;
        });
        surfaces.append(diagnosticText(handle.errors));
        for (const auto &signal : rejected)
            surfaces.append(signal[0].toStringList().join('/') + signal[1].toString());
        QSignalSpy errors(&store, &ConfigStore::sourceError);
        put(file, encodeJsonValue(QVariantMap{{"credentials", invalid}}).bytes);
        result = request(store, [&] { return store.reload(); });
        QVERIFY(result.status == "failed");
        surfaces.append(QString::fromUtf8(encodeJsonValue(qmlResult(result)).bytes));
        for (const auto &signal : errors) {
            const auto error = signal[0].value<ConfigError>();
            surfaces.append(error.path.join('/') + ':' + error.code);
        }
        put(file, malformed);
        result = request(store, [&] { return store.reload(); });
        surfaces.append(QString::fromUtf8(encodeJsonValue(qmlResult(result)).bytes));
        for (const auto &signal : errors)
            surfaces.append(signal[0].value<ConfigError>().path.join('/'));
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        component.setData(
            "import QtQml\nimport ConfigQml 1.0\nConfigStore { required property string file; "
            "autoSave: false;"
            "schema: ConfigSchema { ConfigValue { key: 'credentials'; valueType: ConfigValue.Map;"
            "sensitive: true; admissionPolicy: ConfigValue.Strict; defaultValue: ({});"
            "ConfigValue { valueType: ConfigValue.Integer } } }"
            "sources: [JsonFileSource { path: file }] }",
            QUrl("file:///m5-canary.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const auto qmlFile = dir.filePath("qml.json");
        put(qmlFile, "{}");
        std::unique_ptr<QObject> root(component.createWithInitialProperties({{"file", qmlFile}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *qml = qobject_cast<ConfigQmlStore *>(root.get());
        QTRY_VERIFY(qml->ready());
        QSignalSpy qmlRejected(qml, &ConfigQmlStore::inputRejected);
        auto object = engine.newObject();
        object.setProperty(canary, "invalid");
        surfaces.append(diagnosticText(
            admitQmlInput(o.schema, {"credentials"}, QVariant::fromValue(object)).errors()));
        const auto invoke =
            engine.evaluate("(function(store,key) { try { let v = {}; v[key] = 'invalid';"
                            "store.setUser(['credentials'],v); return 'unexpected'; } catch(e) { "
                            "return String(e); } })");
        surfaces.append(invoke.call({engine.newQObject(qml), canary}).toString());
        for (const auto &signal : qmlRejected)
            surfaces.append(signal[0].toStringList().join('/') + signal[1].toString());
        QSignalSpy qmlErrors(qml, &ConfigQmlStore::sourceError);
        QSignalSpy qmlCompleted(qml, &ConfigQmlStore::completed);
        const auto callback = engine.evaluate(
            "var receipt; (function(result) { receipt = JSON.stringify(result); })");
        put(qmlFile, invalidBytes);
        qml->reload(callback);
        QTRY_VERIFY(!qmlErrors.isEmpty());
        QTRY_VERIFY(!engine.globalObject().property("receipt").isUndefined());
        surfaces.append(engine.globalObject().property("receipt").toString());
        for (const auto &signal : qmlErrors)
            surfaces.append(QString::fromUtf8(encodeJsonValue(signal[0]).bytes));
        for (const auto &signal : qmlCompleted)
            surfaces.append(QString::fromUtf8(encodeJsonValue(signal[0]).bytes));
        auto *detached = qml->scope({});
        detached->detach();
        QSignalSpy detachedRejected(detached, &ConfigQmlStore::inputRejected);
        const auto detachInvoke =
            engine.evaluate("(function(store,key) { try { store.setUser(['credentials',key],1); } "
                            "catch(e) { return String(e); } })");
        surfaces.append(detachInvoke.call({engine.newQObject(detached), canary}).toString());
        for (const auto &signal : detachedRejected)
            surfaces.append(signal[0].toStringList().join('/'));
        for (const auto &surface : surfaces)
            qInfo().noquote() << surface;
        {
            QMutexLocker lock(&messageMutex);
            surfaces.append(messages);
        }
        QVERIFY(surfaces.size() >= 30);
        const auto artifact = qEnvironmentVariable("CONFIGQML_DIAGNOSTIC_ARTIFACT");
        if (!artifact.isEmpty())
            put(artifact, surfaces.join('\n').toUtf8());
        QVERIFY(!surfaces.join('\n').contains(canary));
    }
    void redactionPreservesSchemaIdentity() {
        auto integer = D::leaf(T::SafeInteger, 0);
        auto map = D::leaf(T::Map, QVariantMap{});
        map.element = {integer};
        auto record = D::leaf(T::Record, QVariantMap{{"tokens", QVariantMap{}}});
        record.fields = {{"tokens", map}};
        auto rows = D::list(record, {});
        auto group = D::group({{"rows", rows}, {"direct", map}});
        group.sensitive = true;
        ConfigSchema schema(D::group({{"vault", group}}));
        const auto result =
            schema.validate({"vault", "rows"},
                            QVariantList{QVariantMap{
                                {"tokens", QVariantMap{{"opaque-secret", QString("invalid")}}}}});
        QVERIFY(!result.ok());
        QVERIFY(result.errors.first().path ==
                ConfigPath({"vault", "rows", "<redacted>", "tokens", "<redacted>"}));
        auto elementSensitive = map;
        elementSensitive.element[0].sensitive = true;
        ConfigSchema elementSchema(D::group({{"map", elementSensitive}}));
        const auto nested =
            elementSchema.validate({"map"}, QVariantMap{{"opaque-secret", QString("invalid")}});
        QVERIFY(nested.errors.first().path == ConfigPath({"map", "<redacted>"}));
    }
    void keyTransparentIdentityMigration_data() {
        QTest::addColumn<bool>("naturalAccess");
        QTest::newRow("identity") << false;
        QTest::newRow("natural-access") << true;
    }
    void keyTransparentIdentityMigration() {
        QFETCH(bool, naturalAccess);
        QTemporaryDir dir;
        const auto file = dir.filePath("config.json");
        const QByteArray original =
            "{\"n\":7,\"__proto__\":{\"kept\":true},\"constructor\":\"ctor\","
            "\"prototype\":\"proto\",\"unknown\":18446744073709551615,"
            "\"nested\":[{\"__proto__\":-0,\"constructor\":{\"prototype\":9007199254740993},\"\":"
            "true}],"
            "\"hasOwnProperty\":\"own\",\"toString\":\"own\",\"__defineGetter__\":\"own\"}";
        put(file, original);
        QJSEngine engine;
        ConfigQmlSchema declaration;
        const auto transform =
            naturalAccess
                ? QString(
                      "function(raw) { if (Object.getPrototypeOf(raw) !== null || "
                      "Object.getPrototypeOf(raw.nested[0]) !== null || raw.constructor !== "
                      "\"ctor\" || "
                      "!Object.prototype.hasOwnProperty.call(raw, \"__proto__\")) throw Error(); "
                      "raw.__proto__.kept = true; return raw; }")
                : QString("function(raw) { return raw; }");
        auto step = engine.newObject();
        step.setProperty("fromVersion", 0);
        step.setProperty("transform", transform);
        declaration.migrations = engine.newArray(1);
        declaration.migrations.setProperty(0, step);
        QList<ConfigDiagnostic> errors;
        auto o = options(file);
        o.schema = ConfigSchema(*o.schema.descriptor({}), 1, declaration.compileMigrations(errors));
        QVERIFY(errors.isEmpty());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto after = ConfigJsonCodec().decode(bytes(file), "raw").raw;
        after.remove("_configqmlVersion");
        const auto before = ConfigJsonCodec().decode(original, "raw").raw;
        QVERIFY(after == before);
        QCOMPARE(bytes(file + ".configqml-v0.bak"), original);
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"n"}).toInt(), 7);
    }
    void equalProfileIntentFencesDraft() {
        QTemporaryDir dir;
        ConfigStore store(options(dir.filePath("config.json")));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"n"}, 1);
        request(store, [&] { return store.save(); });
        const auto profile = dir.filePath("profile.json");
        request(store, [&] { return store.exportProfile("One", profile); });
        auto *draft = store.draft({{"n"}});
        QVERIFY(draft->setValue({"n"}, 7).accepted);
        const auto before = store.info({"n"});
        QCOMPARE(request(store, [&] { return store.importProfile(profile); }).status,
                 QString("saved"));
        QVERIFY(store.info({"n"}).contentRevision > before.contentRevision);
        QVERIFY(draft->conflicted());
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                 QString("draft-conflict"));
        QCOMPARE(store.value({"n"}).toInt(), 1);
        QVERIFY(draft->dirty());
    }
    void inheritedStrictNativeInitialization() {
        auto o = options();
        o.layers[0].policy = ConfigAdmissionPolicy::RejectInput;
        o.layers[0].sources[0].raw = {{"n", QString("invalid")}};
        ConfigStore strict(o);
        QTRY_VERIFY(strict.status() != ConfigStore::Status::Loading);
        QCOMPARE(strict.status(), ConfigStore::Status::Error);
        o.layers[0].policy = ConfigAdmissionPolicy::FieldFallback;
        ConfigStore tolerant(o);
        QTRY_COMPARE(tolerant.status(), ConfigStore::Status::Ready);
        QCOMPARE(tolerant.value({"n"}).toInt(), 3);
    }
    void acceptedWriterIntent_data() {
        QTest::addColumn<int>("writer");
        QTest::newRow("direct-equal") << 0;
        QTest::newRow("draft-group-equal") << 1;
        QTest::newRow("profile-equal") << 2;
        QTest::newRow("runtime-equal") << 3;
        QTest::newRow("reset") << 4;
        QTest::newRow("clear-runtime-noop") << 5;
        QTest::newRow("initialize-present-noop") << 6;
    }
    void acceptedWriterIntent() {
        QFETCH(int, writer);
        QTemporaryDir dir;
        auto o = options();
        o.layers.append({"runtime",
                         ConfigLayerKind::Runtime,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"runtime", {}, {}}},
                         false});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"n"}, 1);
        request(store, [&] { return store.save(); });
        const auto profile = dir.filePath("profile.json");
        request(store, [&] { return store.exportProfile("One", profile); });
        auto *old = store.draft({{"n"}});
        old->setValue({"n"}, 7);
        const auto before = store.info({"n"});
        const auto result = request(store, [&]() -> quint64 {
            switch (writer) {
            case 0:
                return store.setUser({"n"}, 1).requestId;
            case 1: {
                auto *other = store.draft({{"n"}});
                other->setValue({"n"}, 1);
                return other->apply();
            }
            case 2:
                return store.importProfile(profile);
            case 3:
                return store.setRuntime({"n"}, 1).requestId;
            case 4:
                return store.reset({"n"}, "user").requestId;
            case 5:
                return store.clearRuntime({"n"}).requestId;
            default:
                return store.initializeIfMissing({"n"}, 1).requestId;
            }
        });
        QVERIFY(result.status != "failed");
        QVERIFY(store.info({"n"}).intentRevision > before.intentRevision);
        if (writer == 2)
            QCOMPARE(store.info({"n"}).pathGeneration, before.pathGeneration);
        QVERIFY(old->conflicted());
        QCOMPARE(request(store, [&] { return old->apply(); }).error.code,
                 QString("draft-conflict"));
        QCOMPARE(store.value({"n"}).toInt(), writer == 4 ? 3 : 1);
        QVERIFY(old->dirty());
    }
    void workerIntentDoesNotForgeIngressGenerations() {
        QTemporaryDir dir;
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"n"}, 1);
        request(store, [&] { return store.save(); });
        const auto profile = dir.filePath("profile.json");
        request(store, [&] { return store.exportProfile("One", profile); });
        auto *old = store.draft({{"n"}});
        old->setValue({"n"}, 7);
        QSignalSpy completed(&store, &ConfigStore::completed);
        ConfigStoreTestAccess::run(store, [](auto &worker) {
            worker.holdInputs(true);
            worker.holdReplies(true);
        });
        const auto imported = store.importProfile(profile);
        const auto direct = store.setUser({"n"}, 9);
        QCOMPARE(store.value({"n"}).toInt(), 9);
        ConfigStoreTestAccess::run(store, [](auto &worker) {
            worker.releaseInputs(1);
            worker.releaseReplies(1);
        });
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed[0][0].value<ConfigResult>().requestId, imported);
        QCOMPARE(store.value({"n"}).toInt(), 9);
        ConfigStoreTestAccess::run(store, [](auto &worker) {
            worker.holdInputs(false);
            worker.holdReplies(false);
        });
        QTRY_COMPARE(completed.size(), 2);
        QCOMPARE(completed[1][0].value<ConfigResult>().requestId, direct.requestId);
        QCOMPARE(store.info({"n"}).latestInput, direct.inputSequence);
        QCOMPARE(store.value({"n"}).toInt(), 9);
        QCOMPARE(request(store, [&] { return old->apply(); }).error.code,
                 QString("draft-conflict"));
    }
    void disjointWorkerIntentDoesNotConflict() {
        QTemporaryDir dir;
        auto o = options();
        o.schema = ConfigSchema(
            D::group({{"n", D::leaf(T::SafeInteger, 3)}, {"other", D::leaf(T::SafeInteger, 3)}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto profile = dir.filePath("profile.json");
        request(store, [&] { return store.exportProfile("Other", profile, {{"other"}}); });
        auto *draft = store.draft({{"n"}});
        draft->setValue({"n"}, 7);
        const auto before = store.info({"n"}).intentRevision;
        request(store, [&] { return store.importProfile(profile); });
        QCOMPARE(store.info({"n"}).intentRevision, before);
        QVERIFY(!draft->conflicted());
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.value({"n"}).toInt(), 7);
    }
    void uniformAdmission_data() {
        QTest::addColumn<int>("adapter");
        QTest::addColumn<int>("nodePolicy");
        QTest::addColumn<bool>("sourceStrict");
        QTest::addColumn<bool>("shipped");
        QTest::addColumn<bool>("parentStrict");
        QTest::addColumn<bool>("reject");
        for (int adapter = 0; adapter < 3; ++adapter) {
            const auto row = [adapter](const char *name, int policy, bool strict, bool shipped,
                                       bool parent, bool reject) {
                const auto label = QByteArray::number(adapter) + '-' + name;
                QTest::newRow(label.constData())
                    << adapter << policy << strict << shipped << parent << reject;
            };
            row("inherit-strict", 0, true, false, false, true);
            row("inherit-tolerant", 0, false, false, false, false);
            row("node-strict", 1, false, false, false, true);
            row("tolerant-source-override", 2, true, false, false, false);
            row("tolerant-parent-override", 2, false, false, true, false);
            row("parent-strict", 0, false, false, true, true);
            row("shipped-strict", 0, false, true, false, true);
            row("shipped-tolerant", 2, false, true, false, false);
        }
    }
    void uniformAdmission() {
        QFETCH(int, adapter);
        QFETCH(int, nodePolicy);
        QFETCH(bool, sourceStrict);
        QFETCH(bool, shipped);
        QFETCH(bool, parentStrict);
        QFETCH(bool, reject);
        if (adapter == 0) {
            auto o = options();
            auto root = *o.schema.descriptor({});
            root.admissionPolicy =
                parentStrict ? D::AdmissionPolicy::Strict : D::AdmissionPolicy::Inherit;
            root.fields["n"].admissionPolicy = D::AdmissionPolicy(nodePolicy);
            o.schema = ConfigSchema(root);
            o.layers[0].policy = sourceStrict ? ConfigAdmissionPolicy::RejectInput
                                              : ConfigAdmissionPolicy::FieldFallback;
            o.layers[0].shippedDefaults = shipped;
            o.layers[0].sources[0].raw = {{"n", QString("invalid")}};
            ConfigLayers layers(o.schema, o.layers);
            QCOMPARE(!layers.rejectedDiagnostics().isEmpty(), reject);
            ConfigStore store(o);
            QTRY_VERIFY(store.status() != ConfigStore::Status::Loading);
            QCOMPARE(store.status(),
                     reject ? ConfigStore::Status::Error : ConfigStore::Status::Ready);
            QCOMPARE(store.value({"n"}).toInt(), 3);
            QVERIFY(store.info({"n"}).provenance.contributions.last().rawPresent);
            QVERIFY(!store.info({"n"}).provenance.contributions.last().valid);
            return;
        }
        QTemporaryDir dir;
        const auto file = dir.filePath("config.json");
        put(file, "{\"n\":\"invalid\"}");
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        const QByteArray type = adapter == 1 ? "MemorySource" : "JsonFileSource";
        const QList<QByteArray> policies{"Inherit", "Strict", "Tolerant"};
        const auto source = type + " { admissionPolicy: " + type +
                            (sourceStrict ? ".RejectInput;" : ".FieldFallback;") +
                            " shippedDefaults: " + (shipped ? "true;" : "false;") +
                            (adapter == 1 ? " data: ({n: 'invalid'})" : " path: file") + " }";
        const auto code = QByteArray("import QtQml\nimport ConfigQml 1.0\nConfigStore { required "
                                     "property string file; autoSave: false;") +
                          "schema: ConfigSchema { " +
                          (parentStrict ? "admissionPolicy: ConfigSchema.Strict;" : "") +
                          "ConfigValue { key: 'n'; valueType: ConfigValue.Integer; defaultValue: "
                          "3; admissionPolicy: ConfigValue." +
                          policies[nodePolicy] + " } } sources: [" + source + "] }";
        component.setData(code, QUrl("file:///m5-policy.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties({{"file", file}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = qobject_cast<ConfigQmlStore *>(root.get());
        QTRY_VERIFY(store->status() != ConfigQmlStore::Loading);
        QCOMPARE(store->status(), reject ? ConfigQmlStore::Error : ConfigQmlStore::Ready);
        QVERIFY(store->core());
        QCOMPARE(store->core()->value({"n"}).toInt(), 3);
        QVERIFY(store->core()->info({"n"}).provenance.contributions.last().rawPresent);
        QVERIFY(!store->core()->info({"n"}).provenance.contributions.last().valid);
    }
};
QTEST_GUILESS_MAIN(M5Boundaries)
#include "test_m5_boundaries.moc"
