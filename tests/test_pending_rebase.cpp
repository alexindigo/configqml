// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "fileoptions.h"
#include "confignumber.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSaveFile>
#include <QProcess>
#include <QSignalSpy>
#include <QDir>
#include <atomic>
#include <QSemaphore>
#include <QScopeGuard>
#include <sys/stat.h>
#include <unistd.h>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
void replace(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        qFatal("atomic replacement fixture");
}
ConfigStoreOptions options(const QString &directory) {
    auto o = fileOptions(directory + "/user.json");
    o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
    o.evidenceDirectory = directory + "/evidence";
    D record(T::Record);
    record.fields = {{"id", D::leaf(T::String, QString())}};
    record.defaultValue = QVariantMap{{"id", QString()}};
    record.open = true;
    D map(T::Map);
    map.element = {record};
    map.defaultValue = QVariantMap{};
    o.schema = ConfigSchema(D::group(
        {{"a", D::leaf(T::SafeInteger, 0)},
         {"b", D::leaf(T::SafeInteger, 0)},
         {"fontDefaultScale", D::leaf(T::Real, 1.1)},
         {"animationDisabled", D::leaf(T::Boolean, false)},
         {"keybinds", D::group({{"keyUp", D::list(D::leaf(T::String, QString()), {QString("F0")})},
                                {"keyDown", D::list(D::leaf(T::String, QString()), {})},
                                {"keyLeft", D::list(D::leaf(T::String, QString()), {})}})},
         {"record", record},
         {"map", map},
         {"items", D::list(record, {})}}));
    o.observedPaths = {{"a"}, {"b"}, {"keybinds", "keyUp"}};
    return o;
}
QVariantMap raw(const QString &path) {
    return ConfigJsonCodec().decode(compat::bytes(path), "json").raw;
}
bool evidenceContains(const QList<ConfigEvidenceReference> &refs, const QString &role,
                      const QByteArray &bytes) {
    for (const auto &ref : refs)
        if (ref.role == role && QFileInfo::exists(ref.directory + "/bytes") &&
            compat::bytes(ref.directory + "/bytes") == bytes)
            return true;
    return false;
}
ConfigRetainedGroupOptions group() { return {"head", {{"b"}}, {{{"b"}, true, 9}}, {}}; }
} // namespace
class PendingRebaseTest : public QObject {
    Q_OBJECT
private slots:
    void unchangedPreimageMatchesStrictSourcePreimage_data() {
        QTest::addColumn<int>("schedule");
        QTest::newRow("equal-intent") << 0;
        QTest::newRow("own-save-ABA") << 1;
        QTest::newRow("absent-reset") << 2;
    }
    void unchangedPreimageMatchesStrictSourcePreimage() {
        QFETCH(int, schedule);
        QList<ConfigResult> receipts;
        QList<QByteArray> bytes;
        for (const auto policy : {ConfigConflictPolicy::StrictSourcePreimage,
                                  ConfigConflictPolicy::StrictPendingRebase}) {
            QTemporaryDir dir;
            const auto path = dir.filePath("user.json");
            if (schedule != 2)
                compat::put(path, schedule == 1 ? "{\"a\":0}\n" : "{\"a\":1}\n");
            auto o = options(dir.path());
            o.jsonFiles[0].conflictPolicy = policy;
            auto entered = std::make_shared<QSemaphore>();
            auto release = std::make_shared<QSemaphore>();
            auto hold = std::make_shared<std::atomic<bool>>(schedule == 1);
            o.fileFault = [=](const QString &phase, const QString &) {
                if (phase == "rename" && hold->exchange(false)) {
                    entered->release();
                    if (!release->tryAcquire(1, 10000))
                        return QString("fixture-timeout");
                }
                return QString();
            };
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            const auto unblock = qScopeGuard([release] { release->release(); });
            quint64 expectedPrefix = 1;
            if (schedule == 2)
                QVERIFY(store.reset({"a"}, "user").accepted);
            else {
                QVERIFY(store.setUser({"a"}, 1).accepted);
                if (schedule == 1) {
                    const auto firstId = store.save();
                    QVERIFY(entered->tryAcquire(1, 5000));
                    QVERIFY(store.setUser({"a"}, 2).accepted);
                    const auto suffix = store.setUser({"a"}, 1);
                    QVERIFY(suffix.accepted);
                    expectedPrefix = suffix.inputSequence;
                    release->release();
                    const auto first = compat::request(store, [&] { return firstId; });
                    QCOMPARE(first.status, QString("saved"));
                    QCOMPARE(first.inputPrefix, quint64(1));
                    QCOMPARE(store.value({"a"}).toInt(), 1);
                    QCOMPARE(store.info({"a"}).latestInput, expectedPrefix);
                    for (const auto &coverage : first.coverage)
                        QVERIFY(coverage.inputThrough < expectedPrefix);
                }
            }
            struct stat before{}, after{};
            const bool existed = ::stat(QFile::encodeName(path).constData(), &before) == 0;
            QCOMPARE(existed, schedule != 2);
            const auto saved = compat::request(store, [&] { return store.save(); });
            QCOMPARE(saved.status, QString("saved"));
            QCOMPARE(saved.inputPrefix, expectedPrefix);
            QCOMPARE(saved.publicationState, QString("known"));
            QCOMPARE(saved.publications.size(), 1);
            const auto &fact = saved.publications[0];
            QVERIFY(fact.success);
            QVERIFY(!fact.rebasedOnto);
            QVERIFY(fact.overriddenPaths.isEmpty());
            QVERIFY(saved.coveredRevisions.contains("json"));
            QVERIFY(::stat(QFile::encodeName(path).constData(), &after) == 0);
            QVERIFY(!existed || before.st_ino != after.st_ino);
            QCOMPARE(compat::bytes(path),
                     schedule == 2 ? QByteArray("{}\n") : QByteArray("{\"a\":1}\n"));
            receipts.append(saved);
            bytes.append(compat::bytes(path));
            ConfigStore restart(o);
            QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
            QCOMPARE(restart.value({"a"}).toInt(), schedule == 2 ? 0 : 1);
        }
        QCOMPARE(bytes[0], bytes[1]);
        const auto &a = receipts[0], &b = receipts[1];
        QCOMPARE(a.status, b.status);
        QCOMPARE(a.coveredRevisions.keys(), b.coveredRevisions.keys());
        QCOMPARE(a.coverage.size(), b.coverage.size());
        QVERIFY(!a.coverage.isEmpty());
        for (qsizetype i = 0; i < a.coverage.size(); ++i) {
            QCOMPARE(a.coverage[i].path, b.coverage[i].path);
            QCOMPARE(a.coverage[i].sourceId, b.coverage[i].sourceId);
            QCOMPARE(a.coverage[i].inputThrough, b.coverage[i].inputThrough);
            QCOMPARE(a.coverage[i].intentRevision, b.coverage[i].intentRevision);
        }
    }
    void sharedParentPublication_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("retained");
        QTest::addColumn<int>("foreign");
        for (int policy : {1, 2})
            for (bool retained : {false, true})
                for (int foreign : {0, 1, 2, 3, 4})
                    QTest::newRow(qPrintable(QString("policy-%1-group-%2-foreign-%3")
                                                 .arg(policy)
                                                 .arg(retained)
                                                 .arg(foreign)))
                        << policy << retained << foreign;
    }
    void sharedParentPublication() {
        QFETCH(int, policy);
        QFETCH(bool, retained);
        QFETCH(int, foreign);
        QTemporaryDir dir;
        const auto settings = dir.filePath("settings");
        const auto deep = settings + "/deep";
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"c", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}, {"c", {"c"}, {}}};
        o.jsonFiles = {{"user", "a", settings + "/a.json"},
                       {"user", "b", deep + "/b.json"},
                       {"user", "c", deep + "/c.json"}};
        for (auto &file : o.jsonFiles)
            file.conflictPolicy = ConfigConflictPolicy(policy);
        o.observedPaths.clear();
        // Retargeting an already accepted configured link must never be covered
        // by the witness for its initially absent resolved directory.
        const auto link = dir.filePath("configured");
        const auto other = dir.filePath("foreign");
        if (foreign == 3) {
            QVERIFY(::symlink(QFile::encodeName(deep).constData(),
                              QFile::encodeName(link).constData()) == 0);
            o.jsonFiles[1].path = link + "/b.json";
            o.jsonFiles[2].path = link + "/c.json";
        }
        auto changed = std::make_shared<std::atomic<bool>>(false);
        o.fileFault = [=](const QString &phase, const QString &path) {
            if (phase != "parent-preflight" ||
                !path.endsWith(foreign == 4 ? "/c.json" : "/b.json") || changed->exchange(true))
                return QString();
            if (foreign == 1) {
                if (!QDir().mkdir(deep))
                    qFatal("foreign mkdir");
                compat::put(deep + "/sentinel", "foreign-data");
            } else if (foreign == 2) {
                if (!QDir().rename(settings, settings + ".own") || !QDir().mkpath(deep))
                    qFatal("foreign parent replacement");
                compat::put(deep + "/sentinel", "foreign-data");
            } else if (foreign == 3) {
                if (!QDir().mkdir(other) || !QFile::rename(link, link + ".own") ||
                    !QFile::link(other, link))
                    qFatal("foreign retarget");
                compat::put(other + "/sentinel", "foreign-data");
            } else if (foreign == 4) {
                if (!QDir().rename(deep, deep + ".own") || !QDir().mkdir(deep))
                    qFatal("foreign witnessed-directory replacement");
                compat::put(deep + "/sentinel", "foreign-data");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(!QFileInfo::exists(settings));
        ConfigResult capture;
        if (retained) {
            capture = compat::request(store, [&] {
                return store.retainGroup(
                    {"first-use",
                     {{"a"}, {"b"}, {"c"}},
                     {{{"a", "n"}, true, 7}, {{"b", "n"}, true, 8}, {{"c", "n"}, true, 9}},
                     {}});
            });
            QCOMPARE(capture.status, QString("retained"));
        } else {
            QVERIFY(store.setUser({"a", "n"}, 7).accepted);
            QVERIFY(store.setUser({"b", "n"}, 8).accepted);
            QVERIFY(store.setUser({"c", "n"}, 9).accepted);
        }
        const auto saved = compat::request(
            store, [&] { return retained ? store.saveGroup(capture.groupToken) : store.save(); });
        QCOMPARE(saved.status, foreign ? QString("failed") : QString("saved"));
        QCOMPARE(saved.publications.size(), 3);
        QCOMPARE(saved.publications[0].sourceId, QString("a"));
        QVERIFY(saved.publications[0].success);
        if (foreign) {
            QCOMPARE(saved.publicationState, QString("partial"));
            for (int i : {1, 2}) {
                if (foreign == 4 && i == 1) {
                    QVERIFY(saved.publications[i].success);
                    QVERIFY(saved.coveredRevisions.contains("b"));
                    continue;
                }
                QVERIFY(!saved.publications[i].success);
                QCOMPARE(saved.publications[i].error.code, QString("source-preimage-conflict"));
                QVERIFY(!saved.coveredRevisions.contains(i == 1 ? "b" : "c"));
            }
            const auto foreignDirectory = foreign == 3 ? dir.filePath("foreign") : deep;
            QCOMPARE(compat::bytes(foreignDirectory + "/sentinel"), QByteArray("foreign-data"));
            QVERIFY(!QFileInfo::exists(foreignDirectory + "/b.json"));
            QVERIFY(!QFileInfo::exists(foreignDirectory + "/c.json"));
            return;
        }
        for (const auto &fact : saved.publications)
            QVERIFY(fact.success);
        QCOMPARE(saved.coveredRevisions.size(), 3);
        QCOMPARE(raw(settings + "/a.json").value("n").value<ConfigNumber>().token(), QString("7"));
        QCOMPARE(raw(deep + "/b.json").value("n").value<ConfigNumber>().token(), QString("8"));
        QCOMPARE(raw(deep + "/c.json").value("n").value<ConfigNumber>().token(), QString("9"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a", "n"}).toInt(), 7);
        QCOMPARE(restart.value({"b", "n"}).toInt(), 8);
        QCOMPARE(restart.value({"c", "n"}).toInt(), 9);
    }
    void declaration() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.evidenceDirectory.clear();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QCOMPARE(store.error(), QString("evidence-directory"));
        QCOMPARE(int(ConfigConflictPolicy::PathIntent), 0);
        QCOMPARE(int(ConfigConflictPolicy::StrictSourcePreimage), 1);
        QCOMPARE(int(ConfigConflictPolicy::StrictPendingRebase), 2);
    }
    void sharedParentWitnessScope() {
        QTemporaryDir dir;
        const auto settings = dir.filePath("settings");
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}};
        o.jsonFiles = {{"user", "a", settings + "/a.json"}, {"user", "b", settings + "/b.json"}};
        for (auto &file : o.jsonFiles)
            file.conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        o.observedPaths.clear();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto a = compat::request(
            store, [&] { return store.retainGroup({"a", {{"a"}}, {{{"a", "n"}, true, 7}}, {}}); });
        const auto b = compat::request(
            store, [&] { return store.retainGroup({"b", {{"b"}}, {{{"b", "n"}, true, 8}}, {}}); });
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(a.groupToken); }).status,
                 QString("saved"));
        // The allowance is transaction-scoped, not a store-lifetime exemption
        // for arbitrary captures made before another save created a parent.
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(b.groupToken); }).error.code,
                 QString("source-preimage-conflict"));
        QVERIFY(!QFileInfo::exists(settings + "/b.json"));
    }
    void atmosphereSchedules_data() {
        QTest::addColumn<int>("schedule");
        QTest::addColumn<bool>("reload");
        for (int schedule = 0; schedule < 3; ++schedule)
            for (bool reload : {false, true})
                QTest::newRow(qPrintable(QString("case-%1-reload-%2").arg(schedule).arg(reload)))
                    << schedule << reload;
    }
    void multiSourceOrigins() {
        QTemporaryDir dir;
        const auto a = dir.filePath("a.json"), b = dir.filePath("b.json");
        compat::put(a, "{\"n\":1}");
        compat::put(b, "{\"n\":1}");
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers[0].sources = {{"a", {"a"}, {}}, {"b", {"b"}, {}}};
        o.jsonFiles = {{"user", "a", a}, {"user", "b", b}};
        for (auto &file : o.jsonFiles)
            file.conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        o.observedPaths = {{"a", "n"}, {"b", "n"}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QList<ConfigSourceAccepted> events;
        connect(&store, &ConfigStore::sourceAccepted, this,
                [&](const auto &event) { events.append(event); });
        QVERIFY(store.setUser({"a", "n"}, 8).accepted);
        QVERIFY(store.setUser({"b", "n"}, 9).accepted);
        replace(b, "{\"n\":4}");
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(events.size(), 3);
        QCOMPARE(events[0].origin, QString("own_save"));
        QCOMPARE(events[0].changedPaths, (QList<ConfigPath>{{"a", "n"}}));
        QCOMPARE(events[1].origin, QString("external"));
        QCOMPARE(events[1].changedPaths, (QList<ConfigPath>{{"b", "n"}}));
        QCOMPARE(events[2].origin, QString("own_save"));
        QCOMPARE(events[2].changedPaths, (QList<ConfigPath>{{"b", "n"}}));
        // Facts are full observed snapshots. A must already have its own event
        // and must not acquire a new fact/revision in B's external generation.
        QCOMPARE(events[0].facts[0].value.toInt(), 8);
        QCOMPARE(events[1].facts[0].value, events[0].facts[0].value);
        QCOMPARE(events[1].facts[0].revision, events[0].facts[0].revision);
        QCOMPARE(events[0].facts[1].value.toInt(), 1);
        QCOMPARE(events[1].facts[1].value.toInt(), 4);
        QCOMPARE(events[2].facts[1].value.toInt(), 9);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a", "n"}).toInt(), 8);
        QCOMPARE(restart.value({"b", "n"}).toInt(), 9);
    }
    void beforeReadFailuresDoNotFreeze_data() {
        QTest::addColumn<bool>("evidence");
        QTest::newRow("lock") << false;
        QTest::newRow("original-evidence") << true;
    }
    void beforeReadFailuresDoNotFreeze() {
        QFETCH(bool, evidence);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        replace(path, "{\"a\":7,\"b\":4}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        if (evidence) {
            QVERIFY(QDir().rename(o.evidenceDirectory, o.evidenceDirectory + ".old"));
            compat::put(o.evidenceDirectory, "blocked");
        } else
            QVERIFY(QDir().mkdir(path + ".lock"));
        const auto failed = compat::request(store, [&] { return store.save(); });
        QCOMPARE(failed.error.code,
                 evidence ? QString("evidence-retention") : QString("lock-failed"));
        if (evidence) {
            QVERIFY(QFile::remove(o.evidenceDirectory));
            QVERIFY(QDir().rename(o.evidenceDirectory + ".old", o.evidenceDirectory));
        } else
            QVERIFY(QDir().rmdir(path + ".lock"));
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QVERIFY(saved.publications[0].rebasedOnto);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 7);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void alreadyAcceptedPreflightIsQuiet() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1,\"_configqmlVersion\":1}");
        auto o = options(dir.path());
        o.schema = ConfigSchema(
            D::group({{"a", D::leaf(T::SafeInteger, 0)}, {"b", D::leaf(T::SafeInteger, 0)}}), 1);
        o.observedPaths.clear();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        replace(path, "{\"a\":7,\"b\":4,\"_configqmlVersion\":1}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QSignalSpy generations(&store, &ConfigStore::published);
        QSignalSpy migrations(&store, &ConfigStore::migrationCompleted);
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(generations.size(), 1);
        QCOMPARE(migrations.size(), 0);
        int foreign = 0;
        for (const auto &reference : saved.evidence)
            foreign += reference.role == "foreign";
        QCOMPARE(foreign, 1);
    }
    void matchingForeignLeafDoesNotWarn_data() {
        QTest::addColumn<bool>("reset");
        QTest::newRow("same-final-value-after-earlier-local-edit") << false;
        QTest::newRow("same-absence") << true;
    }
    void matchingForeignLeafDoesNotWarn() {
        QFETCH(bool, reset);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy warnings(&store, &ConfigStore::sourceWarning);
        QVERIFY(store.setUser({"b"}, 3).accepted);
        if (reset)
            QVERIFY(store.reset({"b"}, "user").accepted);
        else
            QVERIFY(store.setUser({"b"}, 9).accepted);
        QVERIFY(store.setUser({"a"}, 8).accepted);
        replace(path, reset ? QByteArray("{ \"a\": 1 }") : QByteArray("{ \"a\": 1, \"b\": 9 }"));
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QVERIFY(saved.publications[0].rebasedOnto);
        QVERIFY(saved.publications[0].overriddenPaths.isEmpty());
        QCOMPARE(warnings.size(), 0);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 8);
        QCOMPARE(restart.value({"b"}).toInt(), reset ? 0 : 9);
    }
    void invalidConflictPolicy_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("off");
        for (int policy : {-1, 3})
            for (bool off : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(policy).arg(off))) << policy << off;
    }
    void invalidConflictPolicy() {
        QFETCH(int, policy);
        QFETCH(bool, off);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1}");
        auto o = options(dir.path());
        o.evidenceDirectory.clear();
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy(policy);
        o.jsonFiles[0].watchMode = off ? ConfigWatchMode::Off : ConfigWatchMode::StartupSnapshot;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QCOMPARE(store.error(), QString("conflict-policy"));
        ConfigJsonBackend backend(o.jsonFiles);
        const auto id = backend.submit({ConfigBackendRequest::Operation::Read, "json", {}, {}});
        QCOMPARE(backend.take(id)->error.code, QString("conflict-policy"));
    }
    void atmosphereSchedules() {
        QFETCH(int, schedule);
        QFETCH(bool, reload);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray bare = "{\"animationDisabled\":true,\"futureRoot\":[false,0,null,\"\"]}";
        const QByteArray parent =
            "{\"animationDisabled\":true,\"futureRoot\":[false,0,null,\"\"],"
            "\"keybinds\":{\"keyUp\":[\"F1\"],\"keyDown\":[\"F2\"],\"keyLeft\":[\"F2\"],"
            "\"futureSibling\":[false,0,null,\"\"]}}";
        const QByteArray original = schedule == 0   ? QByteArray("{ \"fontDefaultScale\": 1.1 }")
                                    : schedule == 1 ? bare
                                                    : parent;
        const QByteArray foreign =
            schedule == 0   ? QByteArray("{ \"fontDefaultScale\": 1.1, \"totallyUnknown\": true }")
            : schedule == 1 ? parent
                            : bare;
        compat::put(path, original);
        auto o = options(dir.path());
        const QByteArray legacy = "{\"keybinds\":{\"keyUp\":[\"F3\"],\"keyDown\":[\"F4\"]}}";
        compat::put(dir.filePath("legacy.json"), legacy);
        if (schedule == 2) {
            o.layers.prepend({"legacy",
                              ConfigLayerKind::File,
                              ConfigAdmissionPolicy::FieldFallback,
                              {{"legacy", {}, {}}},
                              false});
            o.jsonFiles.prepend({"legacy", "legacy", dir.filePath("legacy.json")});
        }
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QList<ConfigSourceAccepted> events;
        connect(&store, &ConfigStore::sourceAccepted, this,
                [&](const auto &event) { events.append(event); });
        QSignalSpy warnings(&store, &ConfigStore::sourceWarning);
        const ConfigPath leaf =
            schedule == 0 ? ConfigPath{"fontDefaultScale"} : ConfigPath{"keybinds", "keyUp"};
        QVERIFY(store
                    .setUser(leaf,
                             schedule == 0 ? QVariant(1.61) : QVariant(QVariantList{QString("F9")}))
                    .accepted);
        QTRY_COMPARE(store.info(leaf).workerCovered, quint64(1));
        QCOMPARE(compat::bytes(path), original); // Own publication has not happened.
        replace(path, foreign);
        if (reload)
            QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                     QString("reloaded"));
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(saved.publications.size(), 1);
        QVERIFY(saved.publications[0].rebasedOnto);
        const auto expectedPaths =
            schedule == 0 ? QList<ConfigPath>{} : QList<ConfigPath>{{"keybinds", "keyUp"}};
        QCOMPARE(saved.publications[0].overriddenPaths, expectedPaths);
        QCOMPARE(warnings.size(), schedule == 0 ? 0 : 1);
        if (schedule) {
            const auto warning = qvariant_cast<ConfigError>(warnings[0][0]);
            QCOMPARE(warning.code, QString("foreign-value-overridden"));
            QCOMPARE(warning.operation, QString("rebase"));
            QCOMPARE(warning.path, leaf);
            QVERIFY(evidenceContains(warning.evidence, "foreign", foreign));
        }
        QVERIFY(evidenceContains(saved.evidence, "original", original));
        QVERIFY(evidenceContains(saved.evidence, "foreign", foreign));
        const auto output = compat::bytes(path);
        QVERIFY(evidenceContains(saved.evidence, "published", output));
        QVERIFY(evidenceContains(saved.evidence, "readback", output));
        for (const auto &reference : saved.evidence)
            QCOMPARE(raw(reference.directory + "/metadata.json").keys(),
                     (QStringList{"attemptId", "bytesState", "evidenceId", "groupToken",
                                  "observationOrder", "phase", "presence", "requestId", "revision",
                                  "role", "sourceId", "storeId"}));
        QVERIFY(events.size() >= 2);
        QCOMPARE(events.first().origin, QString("external"));
        QCOMPARE(events.last().origin, QString("own_save"));
        QVERIFY(events.first().publicationGeneration < events.last().publicationGeneration);
        const auto result = raw(path);
        if (!schedule) {
            QCOMPARE(result.value("fontDefaultScale").value<ConfigNumber>().real().value.toDouble(),
                     1.61);
            QVERIFY(result.value("totallyUnknown").toBool());
        } else {
            const auto keys = result.value("keybinds").toMap();
            QCOMPARE(keys.value("keyUp").toList(), (QVariantList{QString("F9")}));
            if (schedule == 1) {
                QCOMPARE(keys.value("keyDown").toList(), (QVariantList{QString("F2")}));
                QVERIFY(keys.contains("futureSibling"));
            } else
                QCOMPARE(keys.keys(), (QStringList{"keyUp"}));
            QVERIFY(result.contains("futureRoot"));
        }
        QCOMPARE(compat::bytes(dir.filePath("legacy.json")), legacy);
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value(leaf), store.value(leaf));
        if (schedule)
            QCOMPARE(restart.value({"keybinds", "keyDown"}).toList(),
                     (QVariantList{QString(schedule == 1 ? "F2" : "F4")}));
    }
    void realWriterAndLiveObservation() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        o.jsonFiles[0].watchMode = ConfigWatchMode::Live;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy accepted(&store, &ConfigStore::sourceAccepted);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        QProcess writer;
        writer.start(QCoreApplication::applicationDirPath() + "/file_writer",
                     {path, "a", "7", "manual"});
        QVERIFY(writer.waitForStarted());
        QVERIFY(writer.waitForReadyRead());
        QVERIFY(writer.readAllStandardOutput().contains("READY"));
        writer.write("GO\n");
        QVERIFY(writer.waitForFinished(10000));
        QCOMPARE(writer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(writer.exitCode(), 0);
        QVERIFY(writer.readAllStandardOutput().contains("saved"));
        QCOMPARE(writer.readAllStandardError(), QByteArray());
        QTRY_VERIFY(!accepted.isEmpty());
        QCOMPARE(store.value({"a"}).toInt(), 7);
        QCOMPARE(store.value({"b"}).toInt(), 9);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 7);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void contentEqualABA() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"a\":1,\"b\":1}";
        compat::put(path, original);
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        replace(path, "{\"a\":7,\"b\":4}");
        replace(path, original);
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QVERIFY(!saved.publications[0].rebasedOnto);
        QVERIFY(evidenceContains(saved.evidence, "foreign", original));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void acceptanceMustRetainAndAdmit() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        const auto evidence = o.evidenceDirectory;
        o.fileFault = [evidence](const QString &phase, const QString &) {
            if (phase == "pending-accept") {
                if (!QDir().rename(evidence, evidence + ".old"))
                    qFatal("evidence obstruction");
                compat::put(evidence, "obstructed");
            }
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy accepted(&store, &ConfigStore::sourceAccepted);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        const QByteArray foreign = "{\"a\":7,\"b\":4}";
        replace(path, foreign);
        const auto failed = compat::request(store, [&] { return store.save(); });
        QCOMPARE(failed.error.code, QString("evidence-retention"));
        QCOMPARE(compat::bytes(path), foreign);
        QCOMPARE(accepted.size(), 0);
        QCOMPARE(store.value({"a"}).toInt(), 1);
        QCOMPARE(store.value({"b"}).toInt(), 9);
        // RejectInput admission must likewise prevent preflight acceptance.
        QTemporaryDir rejectedDir;
        const auto rejectedPath = rejectedDir.filePath("user.json");
        compat::put(rejectedPath, "{\"a\":1,\"b\":1}");
        auto rejectedOptions = options(rejectedDir.path());
        rejectedOptions.layers[0].policy = ConfigAdmissionPolicy::RejectInput;
        ConfigStore rejected(rejectedOptions);
        QTRY_COMPARE(rejected.status(), ConfigStore::Status::Ready);
        QVERIFY(rejected.setUser({"b"}, 9).accepted);
        // Pending b masks the invalid foreign b in the presentation candidate;
        // disk admission must still refuse it before a=7 can reach consumers.
        const QByteArray invalid = "{\"a\":7,\"b\":\"invalid\"}";
        replace(rejectedPath, invalid);
        const auto refusal = compat::request(rejected, [&] { return rejected.save(); });
        QCOMPARE(refusal.status, QString("failed"));
        QCOMPARE(refusal.error.sourceId, QString("json"));
        QCOMPARE(refusal.error.operation, QString("admission"));
        QCOMPARE(compat::bytes(rejectedPath), invalid);
        QCOMPARE(rejected.value({"a"}).toInt(), 1);
    }
    void warningCapAndUnchangedOtherPolicies() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        QMap<QString, D> fields;
        QVariantMap original, foreign;
        for (int i = 0; i < 70; ++i) {
            const auto key = QString("n%1").arg(i, 2, 10, QChar('0'));
            fields[key] = D::leaf(T::SafeInteger, 0);
            original[key] = 1;
            foreign[key] = 4;
        }
        ConfigJsonCodec codec;
        compat::put(path, codec.encode(original, "json").bytes);
        auto o = options(dir.path());
        o.schema = ConfigSchema(D::group(fields));
        o.observedPaths.clear();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy warnings(&store, &ConfigStore::sourceWarning);
        for (const auto &key : fields.keys())
            QVERIFY(store.setUser({key}, 9).accepted);
        replace(path, codec.encode(foreign, "json").bytes);
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(saved.publications[0].overriddenPaths.size(), 70);
        QCOMPARE(warnings.size(), 65);
        QCOMPARE(qvariant_cast<ConfigError>(warnings.last()[0]).code,
                 QString("foreign-value-overridden-truncated"));
        for (const auto policy :
             {ConfigConflictPolicy::PathIntent, ConfigConflictPolicy::StrictSourcePreimage}) {
            QTemporaryDir other;
            const auto otherPath = other.filePath("user.json");
            compat::put(otherPath, "{\"a\":1,\"b\":1}");
            auto old = options(other.path());
            old.jsonFiles[0].conflictPolicy = policy;
            ConfigStore unchanged(old);
            QTRY_COMPARE(unchanged.status(), ConfigStore::Status::Ready);
            QVERIFY(unchanged.setUser({"b"}, 9).accepted);
            replace(otherPath, "{\"a\":7,\"b\":4}");
            const auto failed = compat::request(unchanged, [&] { return unchanged.save(); });
            QCOMPARE(failed.error.code, policy == ConfigConflictPolicy::PathIntent
                                            ? QString("path-conflict")
                                            : QString("source-preimage-conflict"));
        }
    }
    void refused_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("code");
        for (const auto &kind : {"scalar", "array", "namespace", "parse", "hardlink", "retarget",
                                 "parent", "lock", "evidence"})
            QTest::newRow(kind) << QString(kind)
                                << QString(QString(kind) == "scalar" || QString(kind) == "array" ||
                                                   QString(kind) == "namespace"
                                               ? "path-conflict"
                                           : QString(kind) == "parse"    ? "json-syntax"
                                           : QString(kind) == "hardlink" ? "hard-linked-target"
                                           : QString(kind) == "lock"     ? "lock-failed"
                                           : QString(kind) == "evidence"
                                               ? "evidence-retention"
                                               : "source-preimage-conflict");
    }
    void refused() {
        QFETCH(QString, kind);
        QFETCH(QString, code);
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("parent")));
        const auto path = dir.filePath("parent/user.json");
        const QByteArray original = "{\"keybinds\":{\"keyUp\":[\"F1\"]}}";
        compat::put(path, original);
        auto o = options(dir.path());
        o.jsonFiles[0].path = path;
        const auto link = dir.filePath("link");
        if (kind == "retarget") {
            QVERIFY(QFile::link(path, link));
            o.jsonFiles[0].path = link;
        }
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        if (kind == "namespace")
            QVERIFY(store.reset({"keybinds"}, "user").accepted);
        else
            QVERIFY(store.setUser({"keybinds", "keyUp"}, QVariantList{QString("F9")}).accepted);
        if (kind == "scalar")
            replace(path, "{\"keybinds\":\"foreign\"}");
        if (kind == "array")
            replace(path, "{\"keybinds\":[]}");
        if (kind == "namespace")
            replace(path, "{\"keybinds\":{\"keyUp\":[\"F1\"],\"new\":true}}");
        if (kind == "parse")
            replace(path, "malformed");
        if (kind == "hardlink")
            QVERIFY(::link(QFile::encodeName(path).constData(),
                           QFile::encodeName(path + ".hard").constData()) == 0);
        if (kind == "retarget") {
            compat::put(dir.filePath("other.json"), original);
            QVERIFY(QFile::rename(link, link + ".old"));
            QVERIFY(QFile::link(dir.filePath("other.json"), link));
        }
        if (kind == "parent") {
            QVERIFY(QDir().rename(dir.filePath("parent"), dir.filePath("old-parent")));
            QVERIFY(QDir().mkpath(dir.filePath("parent")));
            QVERIFY(QFile::rename(dir.filePath("old-parent/user.json"), path));
        }
        if (kind == "lock")
            QVERIFY(QDir().mkdir(path + ".lock"));
        if (kind == "evidence") {
            QVERIFY(QDir().rename(o.evidenceDirectory, o.evidenceDirectory + ".old"));
            compat::put(o.evidenceDirectory, "blocked");
        }
        const auto foreign = compat::bytes(path);
        QSignalSpy warnings(&store, &ConfigStore::sourceWarning);
        const auto failed = compat::request(store, [&] { return store.save(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, code);
        QCOMPARE(compat::bytes(path), foreign);
        QCOMPARE(warnings.size(), 0);
        if (kind == "scalar" || kind == "array" || kind == "namespace") {
            QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                     QString("source-preimage-conflict"));
            QVERIFY(store.setUser({"a"}, 8).accepted);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                     QString("source-preimage-conflict"));
            QCOMPARE(compat::bytes(path), foreign);
            replace(path, original);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
            ConfigStore restart(o);
            QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
            QCOMPARE(restart.value({"a"}).toInt(), 8);
            QCOMPARE(restart.value({"keybinds", "keyUp"}).toList(),
                     (QVariantList{QString(kind == "namespace" ? "F0" : "F9")}));
        }
    }
    void groupsNeverRebaseOrLaunder() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"a\":1,\"b\":1}";
        compat::put(path, original);
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] { return store.retainGroup(group()); });
        replace(path, "{\"a\":7,\"b\":4,\"opaque\":true}");
        QVERIFY(store.setUser({"a"}, 8).accepted);
        const auto ordinary = compat::request(store, [&] { return store.save(); });
        QCOMPARE(ordinary.error.code, QString("unresolved-group"));
        QVERIFY(ordinary.coveredRevisions.contains("json"));
        QCOMPARE(raw(path).value("b").value<ConfigNumber>().token(), QString("4"));
        const auto afterOrdinary = compat::bytes(path);
        const auto failed =
            compat::request(store, [&] { return store.saveGroup(capture.groupToken); });
        QCOMPARE(failed.error.code, QString("source-preimage-conflict"));
        QCOMPARE(
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); }).error.code,
            QString("source-preimage-conflict"));
        QCOMPARE(compat::bytes(path), afterOrdinary);
        replace(path, original);
        QCOMPARE(
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); }).status,
            QString("saved"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 1);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void ownCausalGroupsAndCapturedCleanup() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1,\"keybinds\":{\"keyUp\":[\"F1\"]}}");
        auto o = options(dir.path());
        o.jsonFiles[0].deleteWhenEmpty = true;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto first = compat::request(store, [&] { return store.retainGroup(group()); });
        auto dependent = group();
        dependent.requestKey = "suffix";
        dependent.dependencies = {first.groupToken};
        dependent.edits[0].value = 11;
        const auto second = compat::request(store, [&] { return store.retainGroup(dependent); });
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(first.groupToken); }).status,
                 QString("saved"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(second.groupToken); }).status,
                 QString("saved"));
        const auto reset = compat::request(store, [&] {
            return store.retainGroup({"reset", {{"keybinds"}}, {{{"keybinds"}, false, {}}}, {}});
        });
        QCOMPARE(reset.status, QString("retained"));
        QVERIFY(store.setUser({"a"}, 8).accepted);
        replace(path, "{\"a\":1,\"b\":11,\"keybinds\":{\"keyUp\":[\"F3\"]},\"opaque\":true}");
        const auto ordinary = compat::request(store, [&] { return store.save(); });
        QCOMPARE(ordinary.error.code, QString("unresolved-group"));
        QVERIFY(ordinary.coveredRevisions.contains("json"));
        QCOMPARE(raw(path).value("keybinds").toMap().value("keyUp").toList(),
                 (QVariantList{QString("F3")}));
        QCOMPARE(
            compat::request(store, [&] { return store.saveGroup(reset.groupToken); }).error.code,
            QString("source-preimage-conflict"));
    }
    void frozenRebasedAttempt() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"a\":1,\"b\":1}";
        compat::put(path, original);
        auto o = options(dir.path());
        auto once = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [once](const QString &phase, const QString &) {
            return phase == "write" && once->exchange(false) ? QString("write-fault") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        const QByteArray base = "{\"a\":7,\"b\":4}";
        replace(path, base);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("write-fault"));
        replace(path, "{\"a\":8,\"b\":4}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("source-preimage-conflict"));
        replace(path, base);
        QVERIFY(store.setUser({"fontDefaultScale"}, 1.7).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("source-preimage-conflict"));
        replace(path, original);
        const auto resolved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(resolved.status, QString("saved"));
        QVERIFY(!resolved.publications[0].rebasedOnto);
        QVERIFY(resolved.publications[0].overriddenPaths.isEmpty());
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 1);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
        QCOMPARE(restart.value({"fontDefaultScale"}).toDouble(), 1.7);
    }
    void abaOwnPublicationAdvancesGroup() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"a\":1,\"b\":1}";
        compat::put(path, original);
        auto o = options(dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] { return store.retainGroup(group()); });
        QVERIFY(store.setUser({"a"}, 8).accepted);
        replace(path, original);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("unresolved-group"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(capture.groupToken); }).status,
                 QString("saved"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 8);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void ownCreatedParentsAdvanceGroups() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.jsonFiles[0].path = dir.filePath("new/deep/user.json");
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] { return store.retainGroup(group()); });
        QVERIFY(store.setUser({"a"}, 8).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("unresolved-group"));
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(capture.groupToken); }).status,
                 QString("saved"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 8);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void uncertainOwnOutputAndFrozenOverride_data() {
        QTest::addColumn<bool>("recover");
        QTest::newRow("readback-fault-explicit-own-retry") << false;
        QTest::newRow("restore-preflight-after-uncertain-effect") << true;
    }
    void uncertainOwnOutputAndFrozenOverride() {
        QFETCH(bool, recover);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        auto once = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [once, recover](const QString &phase, const QString &) {
            return phase == (recover ? "directory-sync" : "readback") && once->exchange(false)
                       ? QString("after-effect-fault")
                       : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy warnings(&store, &ConfigStore::sourceWarning);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        const QByteArray foreign = "{\"a\":7,\"b\":4}";
        replace(path, foreign);
        const auto failed = compat::request(store, [&] { return store.save(); });
        QCOMPARE(failed.publicationState, QString("uncertain"));
        QCOMPARE(warnings.size(), 1);
        const auto own = compat::bytes(path);
        if (!recover) {
            const auto retry = compat::request(store, [&] { return store.save(); });
            QCOMPARE(retry.status, QString("saved"));
            QCOMPARE(retry.publications[0].rebasedOnto, failed.publications[0].rebasedOnto);
            QCOMPARE(retry.publications[0].overriddenPaths, (QList<ConfigPath>{{"b"}}));
            QCOMPARE(compat::bytes(path), own);
            ConfigStore restart(o);
            QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
            QCOMPARE(restart.value({"a"}).toInt(), 7);
            QCOMPARE(restart.value({"b"}).toInt(), 9);
            return;
        }
        replace(path, "{\"a\":77,\"b\":44}");
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("source-preimage-conflict"));
        replace(path, foreign);
        warnings.clear();
        const auto resolved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(resolved.status, QString("saved"));
        QCOMPARE(resolved.publications[0].rebasedOnto, failed.publications[0].rebasedOnto);
        QCOMPARE(resolved.publications[0].overriddenPaths, (QList<ConfigPath>{{"b"}}));
        QCOMPARE(warnings.size(), 1);
        QVERIFY(evidenceContains(qvariant_cast<ConfigError>(warnings[0][0]).evidence, "foreign",
                                 foreign));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 7);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void ownGroupAdvancesFrozenOrdinary() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        auto once = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [once](const QString &phase, const QString &) {
            return phase == "write" && once->exchange(false) ? QString("write-fault") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("write-fault"));
        replace(path, "{\"a\":1,\"b\":1}");
        const auto capture = compat::request(
            store, [&] { return store.retainGroup({"a-only", {{"a"}}, {{{"a"}, true, 8}}, {}}); });
        QCOMPARE(compat::request(store, [&] { return store.saveGroup(capture.groupToken); }).status,
                 QString("saved"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"a"}).toInt(), 8);
        QCOMPARE(restart.value({"b"}).toInt(), 9);
    }
    void lateRace_data() {
        QTest::addColumn<bool>("readback");
        QTest::newRow("before-rename") << false;
        QTest::newRow("after-rename") << true;
    }
    void lateRace() {
        QFETCH(bool, readback);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1,\"b\":1}");
        auto o = options(dir.path());
        auto once = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [once, readback](const QString &phase, const QString &file) {
            if (phase == (readback ? "readback" : "rename") && once->exchange(false))
                replace(file, "{\"a\":77,\"b\":77}");
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        replace(path, "{\"a\":2,\"b\":1}");
        const auto failed = compat::request(store, [&] { return store.save(); });
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.publicationState, readback ? QString("uncertain") : QString("known"));
        QCOMPARE(failed.error.code,
                 readback ? QString("readback-mismatch") : QString("source-preimage-conflict"));
        QCOMPARE(compat::bytes(path), QByteArray("{\"a\":77,\"b\":77}"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).error.code,
                 QString("source-preimage-conflict"));
    }
    void conditionalDeletionAndNoop() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"a\":1}");
        auto o = options(dir.path());
        o.jsonFiles[0].deleteWhenEmpty = true;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.initializeIfMissing({"b"}, 9).accepted);
        replace(path, "{\"a\":1,\"b\":4}");
        const auto conditional = compat::request(store, [&] { return store.save(); });
        QCOMPARE(conditional.status, QString("nothing_to_save"));
        QCOMPARE(store.value({"b"}).toInt(), 4);
        QVERIFY(store.setUser({"b"}, 9).accepted);
        replace(path, "{\"a\":1,\"b\":9}\n");
        const auto noop = compat::request(store, [&] { return store.save(); });
        QCOMPARE(noop.status, QString("nothing_to_save"));
        QCOMPARE(noop.publications.size(), 1);
        QVERIFY(noop.publications[0].overriddenPaths.isEmpty());
        QVERIFY(store.reset({}, "user").accepted);
        QVERIFY(QFile::remove(path));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(store.setUser({"b"}, 8).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(store.reset({"b"}, "user").accepted);
        replace(path, "{\"b\":8,\"opaque\":false}");
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(raw(path).keys(), (QStringList{"opaque"}));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"b"}).toInt(), 0);
    }
    void foreignDeletionAndStrictEmpty_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("required");
        for (bool required : {false, true}) {
            QTest::newRow(required ? "required-recreate-only-local" : "recreate-only-local")
                << 0 << required;
            QTest::newRow(required ? "required-leaf-reset-stays-absent" : "leaf-reset-stays-absent")
                << 1 << required;
            QTest::newRow(required ? "required-strict-closure-deletes" : "strict-closure-deletes")
                << 2 << required;
        }
    }
    void sanitizedReceiptPaths() {
        D map(T::Map);
        map.sensitive = true;
        map.element = {D::leaf(T::String, QString())};
        map.defaultValue = QVariantMap{};
        const ConfigSchema schema(D::group({{"private", map}}));
        ConfigResult result;
        ConfigPublication publication;
        publication.overriddenPaths = {{"private", "opaque-key"}};
        result.publications.append(publication);
        QCOMPARE(schema.diagnostics().sanitize(result).publications[0].overriddenPaths,
                 (QList<ConfigPath>{{"private", "<redacted>"}}));
        QCOMPARE(
            schema.diagnostics().at({"private"}).sanitize(result).publications[0].overriddenPaths,
            (QList<ConfigPath>{{"private", "<redacted>"}}));
    }
    void foreignDeletionAndStrictEmpty() {
        QFETCH(int, mode);
        QFETCH(bool, required);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"b\":1}");
        auto o = options(dir.path());
        o.jsonFiles[0].optional = !required;
        if (mode == 2) {
            o.jsonFiles[0].deleteWhenEmpty = true;
            o.layers[0].sources[0].unknownData = ConfigUnknownData::Strict;
        }
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        if (mode)
            QVERIFY(store.reset({"b"}, "user").accepted);
        else
            QVERIFY(store.setUser({"b"}, 9).accepted);
        if (mode == 2)
            replace(path, "{\"b\":1,\"opaque\":false}");
        else
            QVERIFY(QFile::remove(path));
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, mode == 1 ? QString("nothing_to_save") : QString("saved"));
        if (mode)
            QVERIFY(!QFileInfo::exists(path));
        else
            QCOMPARE(raw(path).keys(), (QStringList{"b"}));
        if (mode)
            o.jsonFiles[0].optional = true; // Required is a startup constraint, not a delete veto.
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"b"}).toInt(), mode ? 0 : 9);
    }
    void atomicOpenDataAndStrictClosure_data() {
        QTest::addColumn<bool>("strict");
        QTest::newRow("preserve") << false;
        QTest::newRow("strict") << true;
    }
    void atomicOpenDataAndStrictClosure() {
        QFETCH(bool, strict);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"record\":{\"id\":\"old\"},\"items\":[]}");
        auto o = options(dir.path());
        if (strict)
            o.layers[0].sources[0].unknownData = ConfigUnknownData::Strict;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"record"}, QVariantMap{{"id", QString("local")}}).accepted);
        QVERIFY(store.setUser({"map"}, QVariantMap{{"row", QVariantMap{{"id", QString("local")}}}})
                    .accepted);
        QVERIFY(
            store.setUser({"items"}, QVariantList{QVariantMap{{"id", QString("local")}}}).accepted);
        const QByteArray foreign =
            "{\"record\":{\"id\":\"foreign\",\"openMember\":false},"
            "\"items\":[{\"id\":\"foreign\",\"openMember\":true}],"
            "\"map\":{\"row\":{\"id\":\"foreign\",\"openMember\":true}},\"opaque\":true}";
        replace(path, foreign);
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(saved.publications[0].overriddenPaths,
                 (QList<ConfigPath>{{"record"}, {"map"}, {"items"}}));
        QCOMPARE(raw(path).contains("opaque"), !strict);
        QVERIFY(!raw(path).value("record").toMap().contains("openMember"));
        if (strict)
            QVERIFY(evidenceContains(saved.evidence, "unknown-data", foreign));
        ConfigStore restart(o);
        QTRY_COMPARE(restart.status(), ConfigStore::Status::Ready);
        QCOMPARE(restart.value({"record"}).toMap().value("id").toString(), QString("local"));
        QCOMPARE(restart.value({"map"}).toMap().value("row").toMap().value("id").toString(),
                 QString("local"));
        QCOMPARE(restart.value({"items"}).toList()[0].toMap().value("id").toString(),
                 QString("local"));
    }
};
QTEST_GUILESS_MAIN(PendingRebaseTest)
#include "test_pending_rebase.moc"
