// SPDX-License-Identifier: LGPL-3.0-or-later
// Abandoning a failed admitted draft Apply (native and bound QML).
#include "compatfixture.h"
#include "configdraft.h"
#include "configqmlstore.h"
#include "configreadprojection.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <sys/stat.h>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
struct Fixture {
    QTemporaryDir directory;
    QString file;
    std::shared_ptr<std::atomic_bool> fail = std::make_shared<std::atomic_bool>(true);
    std::unique_ptr<ConfigStore> store;
    Fixture() {
        file = directory.filePath("user.json");
        compat::put(file, "{\"settings\":{\"n\":9,\"other\":10}}");
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"settings", D::group({{"n", D::leaf(T::SafeInteger, 1)},
                                             {"other", D::leaf(T::SafeInteger, 2)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        options.jsonFiles = {{"user", "user", file}};
        options.fileFault = [fail = fail](const QString &phase, const QString &) {
            return fail->load() && phase == "write" ? QString("abandon-write-fault") : QString();
        };
        store = std::make_unique<ConfigStore>(options);
    }
    ConfigResult request(const std::function<quint64(QObject *, ConfigStore::Callback)> &start,
                         int *callbacks = nullptr) {
        ConfigResult result;
        QEventLoop loop;
        const auto id = start(nullptr, [&](const ConfigResult &receipt) {
            result = receipt;
            if (callbacks)
                ++*callbacks;
            loop.quit();
        });
        if (!id)
            qFatal("request refused synchronously");
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        return result;
    }
    ConfigResult applyFailed(ConfigDraft *draft) {
        return request([&](QObject *c, auto f) { return draft->apply(c, f); });
    }
};
} // namespace
class GenericAbandonTest : public QObject {
    Q_OBJECT
private slots:
    void failedApplyAbandonRestoresSaved() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        const auto original = compat::bytes(f.file);
        auto *draft = f.store->draft({{"settings", "n"}});
        QVERIFY(draft->setValue({"settings", "n"}, 5).accepted);
        QCOMPARE(f.applyFailed(draft).status, QString("failed"));
        QCOMPARE(f.store->value({"settings", "n"}).toInt(), 5); // Admitted for Retry.
        const auto before = f.store->info({"settings", "n"}).intentRevision;
        auto *reader = f.store->readProjection();
        QSignalSpy observed(reader, &ConfigReadProjection::operationCompleted);
        QSignalSpy draftPaired(draft->readProjection(), &ConfigReadProjection::operationCompleted);
        int callbacks = 0;
        int valueAtCallback = -1;
        const auto abandoned = f.request(
            [&](QObject *c, auto callback) {
                return draft->abandonApply(c, [&, callback](const ConfigResult &result) {
                    valueAtCallback = f.store->value({"settings", "n"}).toInt();
                    callback(result);
                });
            },
            &callbacks);
        QCOMPARE(abandoned.status, QString("abandoned"));
        QCOMPARE(callbacks, 1);
        QCOMPARE(valueAtCallback, 9); // Readers observe the saved value before completion.
        QCOMPARE(draftPaired.size(), 1);
        QCOMPARE(f.store->value({"settings", "n"}).toInt(), 9);
        QCOMPARE(reader->selectedState({"settings", "n"}).value().toInt(), 9);
        QVERIFY(f.store->info({"settings", "n"}).intentRevision > before);
        QCOMPARE(compat::bytes(f.file), original); // No file is written.
        // The user's staged input stays staged; nothing is pending in the store.
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 5);
        QVERIFY(draft->dirty());
        QVERIFY(!draft->conflicted());
        // Already abandoned: explicit refusal, nothing changes.
        const auto again =
            f.request([&](QObject *c, auto callback) { return draft->abandonApply(c, callback); });
        QCOMPARE(again.status, QString("failed"));
        QCOMPARE(again.error.code, QString("apply-not-pending"));
        QCOMPARE(f.store->value({"settings", "n"}).toInt(), 9);
        // A later Apply is a fresh admission of the staged edit.
        f.fail->store(false);
        const auto applied =
            f.request([&](QObject *c, auto callback) { return draft->apply(c, callback); });
        QCOMPARE(applied.status, QString("saved"));
        QVERIFY(compat::bytes(f.file).contains("\"n\":5"));
        Q_UNUSED(observed);
    }
    void newerChangesAndOtherWritersSurvive() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        const auto original = compat::bytes(f.file);
        auto *draft = f.store->draft({{"settings", "n"}});
        QVERIFY(draft->setValue({"settings", "n"}, 5).accepted);
        QCOMPARE(f.applyFailed(draft).status, QString("failed"));
        // Newer intent on the same leaf and another writer's leaf.
        QCOMPARE(f.request([&](QObject *c, auto callback) {
                      return f.store->setUser({"settings", "n"}, 6, c, callback).requestId;
                  }).status,
                 QString("accepted"));
        QCOMPARE(f.request([&](QObject *c, auto callback) {
                      return f.store->setUser({"settings", "other"}, 11, c, callback).requestId;
                  }).status,
                 QString("accepted"));
        const auto abandoned =
            f.request([&](QObject *c, auto callback) { return draft->abandonApply(c, callback); });
        QCOMPARE(abandoned.status, QString("abandoned"));
        QCOMPARE(f.store->value({"settings", "n"}).toInt(), 6);
        QCOMPARE(f.store->value({"settings", "other"}).toInt(), 11);
        QCOMPARE(compat::bytes(f.file), original);
        f.fail->store(false);
        const auto saved =
            f.request([&](QObject *c, auto callback) { return f.store->save(c, callback); });
        QCOMPARE(saved.status, QString("saved"));
        const auto bytes = compat::bytes(f.file);
        QVERIFY(bytes.contains("\"n\":6"));
        QVERIFY(bytes.contains("\"other\":11"));
    }
    void succeededApplyIsRefused() {
        Fixture f;
        f.fail->store(false);
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *draft = f.store->draft({{"settings", "n"}});
        const auto never =
            f.request([&](QObject *c, auto callback) { return draft->abandonApply(c, callback); });
        QCOMPARE(never.error.code, QString("apply-not-pending"));
        QVERIFY(draft->setValue({"settings", "n"}, 4).accepted);
        QCOMPARE(
            f.request([&](QObject *c, auto callback) { return draft->apply(c, callback); }).status,
            QString("saved"));
        const auto saved = compat::bytes(f.file);
        const auto refused =
            f.request([&](QObject *c, auto callback) { return draft->abandonApply(c, callback); });
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("apply-not-pending"));
        QCOMPARE(f.store->value({"settings", "n"}).toInt(), 4);
        QCOMPARE(compat::bytes(f.file), saved);
    }
    void boundQmlReaderBeforeCallback() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        compat::put(directory.filePath("user.json"), "{\"settings\":{\"n\":9}}");
        const auto original = compat::bytes(directory.filePath("user.json"));
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(ABANDON_FIXTURE));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"directory", directory.path()}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = root->property("store").value<ConfigQmlStore *>();
        QTRY_VERIFY(store && store->ready());
        // A read-only directory makes the atomic replacement fail after admission.
        const auto path = QFile::encodeName(directory.path());
        QCOMPARE(::chmod(path.constData(), 0555), 0);
        QSignalSpy failed(root.get(), SIGNAL(failedApply()));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "run"));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(root->property("failure").toString(), QString());
        QCOMPARE(root->property("bound").toInt(), 5);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "abandon"));
        QTRY_VERIFY(root->property("finished").toBool());
        QCOMPARE(::chmod(path.constData(), 0755), 0);
        QCOMPARE(root->property("abandonStatus").toString(), QString("abandoned"));
        QCOMPARE(root->property("callbacks").toInt(), 1);
        QCOMPARE(root->property("boundAtCallback").toInt(), 9);
        QCOMPARE(root->property("snapshotAtCallback").toInt(), 9);
        QCOMPARE(root->property("secondStatus").toString(), QString("failed"));
        QCOMPARE(root->property("secondCode").toString(), QString("apply-not-pending"));
        QCOMPARE(root->property("bound").toInt(), 9);
        QCOMPARE(compat::bytes(directory.filePath("user.json")), original);
    }
};
QTEST_MAIN(GenericAbandonTest)
#include "test_generic_primitives_abandon.moc"
