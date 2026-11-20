// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configoverlayeditor.h"
#include "configstorehandle.h"
#include "storeprobe.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QSignalSpy>
#include <QEventLoop>
#include <QTimer>
#include <QProcess>
#include <QScopeGuard>
#include <atomic>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
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
ConfigStoreOptions options(const QString &directory, bool fallback = false) {
    auto optional = D(T::String);
    optional.optional = true;
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group({{"settings", D::group({{"n", D::leaf(T::SafeInteger, 1)},
                                                             {"other", D::leaf(T::SafeInteger, 2)},
                                                             {"optional", optional}})},
                                      {"outside", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
    ConfigSource legacy{"legacy", {"settings"}, {{"n", 7}, {"other", 8}}};
    if (fallback)
        legacy.fallbackFor = "user";
    o.layers = {
        {"legacy", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {legacy}, false},
        {"user",
         ConfigLayerKind::File,
         ConfigAdmissionPolicy::FieldFallback,
         {{"user", {"settings"}, {}}, {"outside", {"outside"}, {}}},
         true}};
    o.jsonFiles = {{"user", "user", directory + "/user.json"},
                   {"user", "outside", directory + "/outside.json"}};
    return o;
}
ConfigResult request(ConfigStore &store, const std::function<quint64()> &dispatch) {
    QEventLoop loop;
    ConfigResult result;
    quint64 id = 0;
    const auto connection =
        QObject::connect(&store, &ConfigStore::completed, &loop, [&](const ConfigResult &r) {
            if (r.requestId == id) {
                result = r;
                loop.quit();
            }
        });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    id = dispatch();
    if (!id)
        qFatal("refused request");
    loop.exec();
    QObject::disconnect(connection);
    if (!result.requestId)
        qFatal("request timeout");
    return result;
}
void control(ConfigStore &store, std::function<void(ConfigWorker &)> action) {
    QEventLoop loop;
    bool done = false;
    ConfigStoreTestAccess::run(store, std::move(action), &loop, [&] {
        done = true;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!done)
        qFatal("worker control timeout");
}
} // namespace
class DraftResetTest : public QObject {
    Q_OBJECT
private slots:
    void basicRemoval() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9,\"optional\":\"present\",\"opaque\":18446744073709551615}");
        const auto original = readFile(file);
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings"}});
        QVERIFY(draft->valid());
        const auto before = store.info({"settings", "n"});
        const auto inputs = store.inputs().size();
        const auto staged = request(store, [&] {
            return draft->stageReset({{"settings", "n"}, {"settings", "optional"}});
        });
        QCOMPARE(staged.operation, QString("stageReset"));
        QCOMPARE(staged.status, QString("staged"));
        QVERIFY(staged.coveredRevisions.isEmpty());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 7);
        QVERIFY(!draft->value({"settings", "optional"}).isValid());
        QCOMPARE(store.value({"settings", "n"}).toInt(), 9);
        QCOMPARE(store.info({"settings", "n"}).intentRevision, before.intentRevision);
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(readFile(file), original);
        QVERIFY(draft->discard());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 9);
        QVERIFY(!draft->dirty());
        QCOMPARE(readFile(file), original);
        QCOMPARE(
            request(
                store,
                [&] { return draft->stageReset({{"settings", "n"}, {"settings", "optional"}}); })
                .status,
            QString("staged"));
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(file));
        QVERIFY(raw.ok());
        QVERIFY(!raw.value.toMap().contains("n"));
        QVERIFY(!raw.value.toMap().contains("optional"));
        QVERIFY(raw.value.toMap().contains("opaque"));
        QVERIFY(readFile(file).contains("18446744073709551615"));
        QCOMPARE(store.value({"settings", "n"}).toInt(), 7);
        ConfigStore cold(options(dir.path()));
        QTRY_COMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(cold.value({"settings", "n"}).toInt(), 7);
        QVERIFY(!cold.value({"settings", "optional"}).isValid());
    }
    void wholeSourceCandidateAndSetRefresh() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9}");
        ConfigStore store(options(dir.path(), true));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings"}});
        QCOMPARE(draft->value({"settings", "other"}).toInt(), 2);
        QCOMPARE(request(store, [&] { return draft->stageReset({{"settings", "n"}}); }).status,
                 QString("staged"));
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 7);
        QCOMPARE(draft->previewValue({"settings", "other"}).toInt(), 8);
        // Frozen unedited values stay frozen; the live selected candidate sees
        // source-wide fallback. No previewScope is required for this refresh.
        QCOMPARE(draft->value({"settings", "other"}).toInt(), 2);
        QVERIFY(draft->setValue({"settings", "other"}, 12).accepted);
        QVERIFY(draft->busy());
        QVERIFY(!draft->apply());
        QTRY_VERIFY(!draft->busy());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 1);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 1);
        QCOMPARE(draft->value({"settings", "other"}).toInt(), 12);
        QCOMPARE(request(store, [&] { return draft->stageReset({{"settings", "other"}}); }).status,
                 QString("staged"));
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(draft->value({"settings", "other"}).toInt(), 8);
        QVERIFY(draft->setValue({"settings", "n"}, 1).accepted);
        QTRY_VERIFY(!draft->busy());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 1);
        QCOMPARE(draft->value({"settings", "other"}).toInt(), 2);
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QVERIFY(raw.contains("n")); // Explicit equal-to-default set is not removal.
        QVERIFY(!raw.contains("other"));
    }
    void unknownSiblingSuppressesFallback() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9,\"unknown\":false}");
        ConfigStore store(options(dir.path(), true));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 1);
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QVERIFY(raw.contains("unknown"));
        QVERIFY(!raw.contains("n"));
        QCOMPARE(store.value({"settings", "n"}).toInt(), 1);
    }
    void selectionValidation() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9,\"other\":10}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QVERIFY(draft->setValue({"settings", "n"}, 11).accepted);
        const QList<QList<ConfigPath>> invalid{{},
                                               {{"settings", "other"}},
                                               {{"absent"}},
                                               {{"settings", "n"}, {"settings", "n"}},
                                               {{"settings", "n"}, {"outside"}}};
        for (const auto &selection : invalid) {
            QCOMPARE(request(store, [&] { return draft->stageReset(selection); }).status,
                     QString("failed"));
            QCOMPARE(draft->value({"settings", "n"}).toInt(), 11);
            QVERIFY(!draft->busy());
        }
        QCOMPARE(
            request(store, [&] { return draft->stageReset({{"settings"}, {"settings", "n"}}); })
                .status,
            QString("staged"));
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QVERIFY(!raw.contains("n"));
        QVERIFY(raw.contains("other"));
    }
    void selectedPublication() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9,\"other\":10}");
        writeFile(dir.filePath("outside.json"), "{\"n\":4}");
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        QCOMPARE(request(store, [&] { return store.setUser({"settings", "other"}, 20).requestId; })
                     .status,
                 QString("accepted"));
        QCOMPARE(
            request(store, [&] { return store.setUser({"outside", "n"}, 30).requestId; }).status,
            QString("accepted"));
        const auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(readFile(dir.filePath("outside.json")), QByteArray("{\"n\":4}"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QCOMPARE(encodeJsonValue(raw.value("other")).bytes, QByteArray("10"));
        QVERIFY(!raw.contains("n"));
        QCOMPARE(result.coverage.size(), 1);
        QCOMPARE(store.value({"settings", "other"}).toInt(), 20);
        QCOMPARE(store.value({"outside", "n"}).toInt(), 30);
    }
    void foreignPreviewAndFrozenValues() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9}");
        writeFile(dir.filePath("legacy.json"), "{\"n\":7,\"other\":8}");
        auto o = options(dir.path());
        o.jsonFiles.prepend({"legacy", "legacy", dir.filePath("legacy.json")});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{"settings", "n"}}); }).status,
                 QString("staged"));
        writeFile(dir.filePath("legacy.json"), "{\"n\":17,\"other\":18}");
        QCOMPARE(request(store, [&] { return store.reload(); }).status, QString("reloaded"));
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 17);
        QCOMPARE(draft->value({"settings", "other"}).toInt(), 8);
        QCOMPARE(draft->previewValue({"settings", "other"}).toInt(), 18);
        QVERIFY(draft->conflicted());
        QCOMPARE(
            request(store, [&] { return draft->stageReset({{"settings", "other"}}); }).error.code,
            QString("draft-conflict"));
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        // A failed stage must not disable the previous candidate's live refresh.
        writeFile(dir.filePath("legacy.json"), "{\"n\":27,\"other\":28}");
        QCOMPARE(request(store, [&] { return store.reload(); }).status, QString("reloaded"));
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 27);
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                 QString("draft-conflict"));
        QVERIFY(draft->dirty());
        QVERIFY(draft->discard());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 9);
    }
    void stalePresentationCannotReplaceSetOrDiscard() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto saved = std::make_shared<ConfigWorkerPacket>();
        control(store, [saved](ConfigWorker &worker) {
            QObject::connect(&worker, &ConfigWorker::packetReady, &worker,
                             [saved](const ConfigWorkerPacket &packet) {
                                 if (saved->drafts.isEmpty() && !packet.drafts.isEmpty())
                                     *saved = packet;
                             });
        });
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        QVERIFY(!saved->drafts.isEmpty());
        QVERIFY(draft->setValue({"settings", "n"}, 13).accepted);
        QTRY_VERIFY(!draft->busy());
        ConfigStoreTestAccess::inject(store, *saved);
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 13);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 13);
        QVERIFY(draft->discard());
        QVERIFY(draft->setValue({"settings", "n"}, 14).accepted);
        ConfigStoreTestAccess::inject(store, *saved);
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 14);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 14);
        QCOMPARE(store.value({"settings", "n"}).toInt(), 9);
    }
    void busyAndDestroyedDelivery() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        control(store, [](ConfigWorker &w) { w.holdReplies(true); });
        bool callback = false;
        const auto staged = draft->stageReset({{}}, this, [&](const auto &) { callback = true; });
        QVERIFY(staged);
        QVERIFY(draft->busy());
        QVERIFY(!draft->setValue({"settings", "n"}, 21).accepted);
        QVERIFY(!draft->discard());
        QVERIFY(!draft->apply());
        QVERIFY(!draft->stageReset({{}}));
        delete draft;
        QSignalSpy completed(&store, &ConfigStore::completed);
        control(store, [](ConfigWorker &w) { w.holdReplies(false); });
        QTRY_VERIFY(!completed.isEmpty());
        QVERIFY(!callback);
        QCOMPARE(store.value({"settings", "n"}).toInt(), 9);
        QCOMPARE(store.inputs().size(), 0);
        QCOMPARE(readFile(file), QByteArray("{\"n\":9}"));
    }
    void failureRetryRetainsRemoval_data() {
        QTest::addColumn<QString>("faultPhase");
        QTest::newRow("before-publication") << QString("write");
        QTest::newRow("after-publication") << QString("directory-sync");
        QTest::newRow("readback") << QString("readback");
    }
    void failureRetryRetainsRemoval() {
        QFETCH(QString, faultPhase);
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9}");
        auto o = options(dir.path());
        auto fail = std::make_shared<std::atomic_bool>(true);
        o.evidenceDirectory = dir.filePath("evidence");
        o.fileFault = [fail, faultPhase](const QString &phase, const QString &) {
            return fail->load() && phase == faultPhase ? QString("staged-reset-fault") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        const auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("staged-reset-fault"));
        QVERIFY(draft->dirty());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(store.value({"settings", "n"}).toInt(), 7);
        const auto inputs = store.inputs().size();
        const auto intent = store.info({"settings", "n"}).intentRevision;
        fail->store(false);
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info({"settings", "n"}).intentRevision, intent);
        QVERIFY(!draft->dirty());
        QVERIFY(!decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap().contains("n"));
    }
    void failedRemovalDoesNotResumeAutosave() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9,\"other\":10}");
        const auto original = readFile(file);
        auto o = options(dir.path());
        o.autoSave = true;
        o.autoSaveInterval = 25;
        auto fail = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [fail](const QString &phase, const QString &) {
            return fail->load() && phase == "write" ? QString("staged-reset-fault") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("failed"));
        const auto inputs = store.inputs().size();
        fail->store(false);
        QTest::qWait(250);
        QCOMPARE(readFile(file), original);
        QVERIFY(draft->dirty());
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.inputs().size(), inputs);
        QVERIFY(!decodeJsonValue(readFile(file)).value.toMap().contains("n"));
    }
    void shapesPresenceAndAtomicRemoval() {
        QTemporaryDir dir;
        auto record = D::leaf(T::Record, QVariantMap{{"label", "schema"}});
        record.open = true;
        record.fields = {{"label", D::leaf(T::String, QString())}};
        auto map = D::leaf(T::Map, QVariantMap{});
        map.element = {D::leaf(T::String, QString())};
        auto nullable = D::leaf(T::String, QString("schema"));
        nullable.nullable = true;
        const QMap<QString, D> fields{
            {"flag", D::leaf(T::Boolean, true)},
            {"zero", D::leaf(T::SafeInteger, 5)},
            {"text", D::leaf(T::String, QString("schema"))},
            {"list", D::list(D::leaf(T::String, QString()), {QString("schema")})},
            {"record", record},
            {"emptyRecord", D::leaf(T::Record, QVariantMap{})},
            {"map", map},
            {"nullable", nullable}};
        const auto lower = decodeJsonValue("{\"flag\":false,\"zero\":0,\"text\":\"\",\"list\":[],"
                                           "\"record\":{\"label\":\"lower\",\"wide\":"
                                           "18446744073709551615,\"__proto__\":{\"x\":false}},"
                                           "\"emptyRecord\":{},\"map\":{},\"nullable\":null}")
                               .value.toMap();
        writeFile(
            dir.filePath("user.json"),
            "{\"flag\":true,\"zero\":9,\"text\":\"chosen\",\"list\":[\"chosen\"],"
            "\"record\":{\"label\":\"chosen\",\"ownUnknown\":123},\"emptyRecord\":{\"opaque\":true}"
            ","
            "\"map\":{\"chosen\":\"chosen\"},\"nullable\":\"chosen\",\"outsideUnknown\":false}");
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"settings", D::group(fields)}}));
        o.layers[0].sources[0].raw = lower;
        o.layers[1].sources = {o.layers[1].sources.first()};
        o.jsonFiles = {o.jsonFiles.first()};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings"}});
        QCOMPARE(
            request(store, [&] { return draft->stageReset({{"settings", "record", "label"}}); })
                .status,
            QString("failed"));
        QCOMPARE(request(store, [&] { return draft->stageReset({{"settings"}}); }).status,
                 QString("staged"));
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            const ConfigPath path{"settings", it.key()};
            const auto expected =
                o.schema.validate(path, jsonSettingValue(lower.value(it.key()), *it));
            QVERIFY(expected.ok());
            QVERIFY(draft->value(path).isValid());
            QCOMPARE(draft->value(path), expected.value);
            QCOMPARE(draft->previewValue(path), expected.value);
        }
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QCOMPARE(raw.size(), 1);
        QVERIFY(raw.contains("outsideUnknown"));
        QCOMPARE(
            encodeJsonValue(draft->value({"settings", "record"})).bytes,
            encodeJsonValue(o.schema.validate({"settings", "record"}, lower.value("record")).value)
                .bytes);
    }
    void maskedRemovalAndPreflightConflict() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9}");
        auto o = options(dir.path());
        o.layers.append({"runtime",
                         ConfigLayerKind::Runtime,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"runtime", {"settings"}, {{"n", 99}}}},
                         false});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 99);
        QCOMPARE(draft->previewValue({"settings", "n"}).toInt(), 99);
        // StartupSnapshot leaves this foreign raw change unknown until Apply.
        writeFile(dir.filePath("user.json"), "{\"n\":17}");
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                 QString("draft-conflict"));
        QVERIFY(draft->dirty());
        QCOMPARE(readFile(dir.filePath("user.json")), QByteArray("{\"n\":17}"));
        QVERIFY(draft->discard());
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        const auto result = request(store, [&] { return draft->apply(); });
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(result.maskedBy, QString("runtime"));
        QCOMPARE(store.value({"settings", "n"}).toInt(), 99);
        QVERIFY(!decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap().contains("n"));
    }
    void rejectedSiblingCanBeRepairedWithoutDiscard() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9,\"other\":10}");
        auto o = options(dir.path());
        o.admission = [](const ConfigInput &input) {
            return input.kind == ConfigInput::Kind::Reset &&
                           input.path == ConfigPath{"settings", "n"}
                       ? QString("reset-refused")
                       : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings"}});
        QCOMPARE(
            request(store,
                    [&] { return draft->stageReset({{"settings", "n"}, {"settings", "other"}}); })
                .status,
            QString("staged"));
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                 QString("reset-refused"));
        QVERIFY(draft->dirty());
        QVERIFY(draft->setValue({"settings", "n"}, 11).accepted);
        QTRY_VERIFY(!draft->busy());
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QVERIFY(raw.contains("n"));
        QVERIFY(!raw.contains("other"));
        QCOMPARE(store.value({"settings", "n"}).toInt(), 11);
    }
    void partialRetryAndOverlappingReservation() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9}");
        writeFile(dir.filePath("outside.json"), "{\"n\":4}");
        auto o = options(dir.path());
        auto fail = std::make_shared<std::atomic_bool>(true);
        o.fileFault = [fail](const QString &phase, const QString &file) {
            return fail->load() && phase == "write" && file.endsWith("outside.json")
                       ? QString("partial-reset-fault")
                       : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}, {"outside", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        const auto partial = request(store, [&] { return draft->apply(); });
        QCOMPARE(partial.status, QString("failed"));
        QCOMPARE(partial.publicationState, QString("partial"));
        QCOMPARE(partial.coveredRevisions.keys(), QStringList{"user"});
        QVERIFY(draft->dirty());
        const auto count = store.inputs().size();
        fail->store(false);
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.inputs().size(), count);
        // An overlapping reservation refuses before admission, not just after IO.
        auto *next = store.draft({{"settings", "n"}});
        const auto retained = request(
            store, [&] { return store.retainGroup({"reservation", {{"settings", "n"}}, {}, {}}); });
        QCOMPARE(retained.status, QString("retained"));
        QCOMPARE(request(store, [&] { return next->stageReset({{}}); }).status, QString("staged"));
        const auto revision = store.info({"settings", "n"}).intentRevision;
        QCOMPARE(request(store, [&] { return next->apply(); }).error.code,
                 QString("unresolved-group"));
        QCOMPARE(store.info({"settings", "n"}).intentRevision, revision);
        QVERIFY(next->dirty());
        QCOMPARE(request(store, [&] { return store.saveGroup(retained.groupToken); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(request(store, [&] { return store.releaseGroup(retained.groupToken); }).status,
                 QString("released"));
    }
    void unrelatedStoppedGroupStaysUnpublished() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9,\"other\":10}");
        writeFile(dir.filePath("outside.json"), "{\"n\":4}");
        auto o = options(dir.path());
        o.fileFault = [](const QString &phase, const QString &file) {
            return phase == "write" && file.endsWith("outside.json") ? QString("stopped-group")
                                                                     : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(
            request(store, [&] { return store.setUser({"outside", "n"}, 30).requestId; }).status,
            QString("accepted"));
        const auto retained =
            request(store, [&] { return store.retainGroup({"stopped", {{"outside"}}, {}, {}}); });
        QCOMPARE(retained.status, QString("retained"));
        const auto failed = request(store, [&] { return store.saveGroup(retained.groupToken); });
        QCOMPARE(failed.error.code, QString("stopped-group"));
        const auto stopped = store.groupInfo(retained.groupToken);
        QCOMPARE(stopped.state, QString("stopped"));
        QCOMPARE(request(store, [&] { return store.setUser({"settings", "other"}, 20).requestId; })
                     .status,
                 QString("accepted"));
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        const auto saved = request(store, [&] { return draft->apply(); });
        QCOMPARE(saved.status, QString("saved"));
        QVERIFY(saved.excludedGroups.contains(retained.groupToken));
        const auto after = store.groupInfo(retained.groupToken);
        QCOMPARE(after.state, stopped.state);
        QCOMPARE(after.capturePrefix, stopped.capturePrefix);
        QCOMPARE(after.attempts.size(), stopped.attempts.size());
        QCOMPARE(after.attempts.last().attemptId, stopped.attempts.last().attemptId);
        QCOMPARE(readFile(dir.filePath("outside.json")), QByteArray("{\"n\":4}"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QVERIFY(!raw.contains("n"));
        QCOMPARE(encodeJsonValue(raw.value("other")).bytes, QByteArray("10"));
    }
    void unwritableTargetAndRetry() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        const auto permissions = QFile::permissions(dir.path());
        const auto restore = qScopeGuard([&] { QFile::setPermissions(dir.path(), permissions); });
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
        const auto failed = request(store, [&] { return draft->apply(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.publicationState, QString("known"));
        QVERIFY(failed.coveredRevisions.isEmpty());
        QVERIFY(draft->dirty());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(readFile(file), QByteArray("{\"n\":9}"));
        const auto count = store.inputs().size();
        const auto intent = store.info({"settings", "n"}).intentRevision;
        QVERIFY(QFile::setPermissions(dir.path(), permissions));
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.inputs().size(), count);
        QCOMPARE(store.info({"settings", "n"}).intentRevision, intent);
        QVERIFY(!decodeJsonValue(readFile(file)).value.toMap().contains("n"));
    }
    void competingIntentAlwaysFencesRemoval_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("equal") << 0;
        QTest::newRow("changed") << 1;
        QTest::newRow("ABA") << 2;
        QTest::newRow("equal-after-failed-apply") << 3;
    }
    void competingIntentAlwaysFencesRemoval() {
        QFETCH(int, kind);
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9}");
        auto o = options(dir.path());
        auto fail = std::make_shared<std::atomic_bool>(kind == 3);
        o.fileFault = [fail](const QString &phase, const QString &) {
            return *fail && phase == "write" ? QString("intent-fault") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        if (kind == 3) {
            QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                     QString("intent-fault"));
            fail->store(false);
        }
        const auto current = store.value({"settings", "n"});
        QCOMPARE(request(store,
                         [&] {
                             return store
                                 .setUser({"settings", "n"},
                                          kind == 1 || kind == 2 ? QVariant(19) : current)
                                 .requestId;
                         })
                     .status,
                 QString("accepted"));
        if (kind == 2)
            QCOMPARE(
                request(store, [&] { return store.setUser({"settings", "n"}, current).requestId; })
                    .status,
                QString("accepted"));
        const auto before = store.inputs().size();
        QCOMPARE(request(store, [&] { return draft->apply(); }).error.code,
                 QString("draft-conflict"));
        QVERIFY(draft->dirty());
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(readFile(file), QByteArray("{\"n\":9}"));
        QCOMPARE(store.inputs().size(), before + (kind == 3 ? 0 : 1));
    }
    void callbackReentryAndDeadContext() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        bool signalled = false, called = false;
        connect(&store, &ConfigStore::completed, this, [&](const ConfigResult &r) {
            if (r.operation == "stageReset")
                signalled = true;
        });
        QVERIFY(draft->stageReset({{}}, this, [&](const ConfigResult &result) {
            called = true;
            QCOMPARE(result.status, QString("staged"));
            QVERIFY(signalled);
            QVERIFY(!draft->busy());
            QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
            QVERIFY(draft->discard());
            QVERIFY(draft->setValue({"settings", "n"}, 15).accepted);
        }));
        QTRY_VERIFY(called);
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 15);
        QVERIFY(draft->discard());
        control(store, [](ConfigWorker &w) { w.holdReplies(true); });
        auto *context = new QObject;
        bool deadCalled = false;
        QVERIFY(draft->stageReset({{}}, context, [&](const auto &) { deadCalled = true; }));
        delete context;
        control(store, [](ConfigWorker &w) { w.holdReplies(false); });
        QTRY_VERIFY(!draft->busy());
        QVERIFY(!deadCalled);
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QCOMPARE(readFile(file), QByteArray("{\"n\":9}"));
    }
    void applySurvivesPresentationDestruction() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9}");
        ConfigStore store(options(dir.path()));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings", "n"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{}}); }).status, QString("staged"));
        control(store, [](ConfigWorker &w) { w.holdReplies(true); });
        bool called = false;
        const auto id = draft->apply(this, [&](const auto &) { called = true; });
        QVERIFY(id);
        delete draft;
        QSignalSpy results(&store, &ConfigStore::completed);
        control(store, [](ConfigWorker &w) { w.holdReplies(false); });
        QTRY_VERIFY(!results.isEmpty());
        QCOMPARE(results.last().first().value<ConfigResult>().requestId, id);
        QCOMPARE(results.last().first().value<ConfigResult>().status, QString("saved"));
        QVERIFY(!called);
        QVERIFY(!decodeJsonValue(readFile(file)).value.toMap().contains("n"));
        QCOMPARE(store.value({"settings", "n"}).toInt(), 7);
    }
    void candidateProjectionEffectsStayPrivate() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        writeFile(file, "{\"n\":9}");
        auto o = options(dir.path());
        auto runs = std::make_shared<std::atomic_int>(0);
        o.projections = {{"effects", {"user"}, {}, [runs](const ConfigProjectionInput &input) {
                              ++*runs;
                              return ConfigProjectionResult{input.values,
                                                            input.compatibilityRaw,
                                                            {{"effect", 1, "action", {}}},
                                                            {}};
                          }}};
        ConfigStore store(o);
        QSignalSpy effects(&store, &ConfigStore::projectionEffects);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(!effects.isEmpty());
        effects.clear();
        const auto baselineRuns = runs->load();
        const auto revision = store.info({"settings", "n"}).intentRevision;
        auto *draft = store.draft({{"settings"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{"settings", "n"}}); }).status,
                 QString("staged"));
        QVERIFY(runs->load() > baselineRuns); // The real projection ran on the candidate.
        QVERIFY(effects.isEmpty());
        QVERIFY(draft->setValue({"settings", "other"}, 12).accepted);
        QTRY_VERIFY(!draft->busy());
        QVERIFY(effects.isEmpty());
        QVERIFY(draft->discard());
        QVERIFY(effects.isEmpty());
        QVERIFY(store.inputs().isEmpty());
        QCOMPARE(store.info({"settings", "n"}).intentRevision, revision);
        QCOMPARE(readFile(file), QByteArray("{\"n\":9}"));
    }
    void candidateRefreshFailureRetainsAttemptedInput() {
        QTemporaryDir dir;
        writeFile(dir.filePath("user.json"), "{\"n\":9,\"record\":{\"label\":\"user\"}}");
        auto o = options(dir.path());
        auto root = *o.schema.descriptor({});
        auto record = D::leaf(T::Record, QVariantMap{{"label", "schema"}});
        record.fields = {{"label", D::leaf(T::String, QString())}};
        root.fields["settings"].fields["record"] = record;
        o.schema = ConfigSchema(root);
        o.layers[1].sources[0].unknownData = ConfigUnknownData::Strict;
        o.evidenceDirectory = dir.filePath("evidence");
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"settings"}});
        QCOMPARE(request(store, [&] { return draft->stageReset({{"settings", "n"}}); }).status,
                 QString("staged"));
        const QVariantMap attempted{{"label", "attempted"}, {"extra", true}};
        QVERIFY(draft->setValue({"settings", "record"}, attempted).accepted);
        QTRY_VERIFY(!draft->busy());
        QCOMPARE(draft->error(), QString("undeclared-member"));
        QCOMPARE(draft->value({"settings", "record"}).toMap(), attempted);
        QCOMPARE(draft->value({"settings", "n"}).toInt(), 7);
        QVERIFY(!draft->apply());
        QVERIFY(draft->setValue({"settings", "record"}, QVariantMap{{"label", "fixed"}}).accepted);
        QTRY_VERIFY(!draft->busy());
        QVERIFY(draft->error().isEmpty());
        QCOMPARE(request(store, [&] { return draft->apply(); }).status, QString("saved"));
        const auto raw = decodeJsonValue(readFile(dir.filePath("user.json"))).value.toMap();
        QVERIFY(!raw.contains("n"));
        QCOMPARE(raw.value("record").toMap().value("label").toString(), QString("fixed"));
    }
};
QTEST_GUILESS_MAIN(DraftResetTest)
#include "test_draft_reset.moc"
