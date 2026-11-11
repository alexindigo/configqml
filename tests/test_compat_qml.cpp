// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "compatfixture.h"
#include "confignumber.h"
#include "configqmloverlay.h"
#include <QQmlEngine>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QDir>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QScopeGuard>
using namespace configqml;
namespace {
// A directory cannot be opened as an O_RDWR regular lock file, even with DAC override.
// Preserve any existing sidecar and restore only our own obstruction on every exit.
class LockObstruction {
public:
    explicit LockObstruction(QString path) : m_path(std::move(path)), m_saved(m_path + ".saved") {}
    ~LockObstruction() {
        if (!restore())
            qFatal("lock obstruction restoration failed");
    }
    bool block() {
        if (m_blocked || m_preserved || QFileInfo::exists(m_saved))
            return false;
        if (QFileInfo::exists(m_path)) {
            if (!QFileInfo(m_path).isFile() || !QFile::rename(m_path, m_saved))
                return false;
            m_preserved = true;
        }
        m_blocked = QDir().mkdir(m_path);
        return m_blocked;
    }
    bool restore() {
        if (m_blocked) {
            if (!QDir().rmdir(m_path))
                return false;
            m_blocked = false;
        }
        if (m_preserved) {
            if (!QFile::rename(m_saved, m_path))
                return false;
            m_preserved = false;
        }
        return true;
    }

private:
    QString m_path, m_saved;
    bool m_blocked = false, m_preserved = false;
};
} // namespace
class CompatibilityQmlTest : public QObject {
    Q_OBJECT
private slots:
    void boundConsumer_data() {
        QTest::addColumn<int>("crossSchedule");
        QTest::addColumn<QString>("numberLeg");
        QTest::addColumn<QString>("numberMode");
        if (QString(COMPAT_FIXTURE).endsWith("tst_list_own_save.qml")) {
            // This fixture uses the shared integer case selector, not a watch schedule.
            QTest::newRow("no-save") << 0 << QString() << QString();
            QTest::newRow("saved-original") << 1 << QString() << QString();
            QTest::newRow("saved-fresh-control") << 2 << QString() << QString();
            return;
        }
        if (QString(COMPAT_FIXTURE).endsWith("tst_overlay_number.qml")) {
            for (const auto *leg : {"integer", "fraction", "json", "absent", "string"})
                for (const auto *mode : {"live", "editor", "view"})
                    QTest::newRow(qPrintable(QString("%1-%2").arg(leg, mode)))
                        << 0 << QString::fromLatin1(leg) << QString::fromLatin1(mode);
            return;
        }
        if (QString(COMPAT_FIXTURE).endsWith("tst_source_events.qml")) {
            QTest::newRow("back-to-back") << 0 << QString() << QString();
            QTest::newRow("80ms-gap") << 80 << QString() << QString();
            QTest::newRow("watch-separated") << -1 << QString() << QString();
        } else
            QTest::newRow("default") << 0 << QString() << QString();
    }
    void boundConsumer() {
        QFETCH(int, crossSchedule);
        QFETCH(QString, numberLeg);
        QFETCH(QString, numberMode);
        QTemporaryDir dir;
        compat::put(dir.filePath("legacy.json"),
                    "{\"oldCount\":7,\"a\":1,\"b\":2,\"wide\":18446744073709551615}");
        compat::put(dir.filePath("bad.json"), "malformed");
        const bool retained = QString(COMPAT_FIXTURE).endsWith("tst_retained_groups.qml");
        const bool strictEmpty = QString(COMPAT_FIXTURE).endsWith("tst_strict_empty.qml");
        const bool overlay = QString(COMPAT_FIXTURE).endsWith("tst_overlay_editor.qml");
        const bool liveOverlay = QString(COMPAT_FIXTURE).endsWith("tst_live_overlay.qml");
        const bool draftPreview = QString(COMPAT_FIXTURE).endsWith("tst_draft_preview.qml");
        const bool draftRetry = QString(COMPAT_FIXTURE).endsWith("tst_draft_retry.qml");
        const bool overlayNumber = QString(COMPAT_FIXTURE).endsWith("tst_overlay_number.qml");
        const bool listOwnSave = QString(COMPAT_FIXTURE).endsWith("tst_list_own_save.qml");
        const bool notifyOrder = QString(COMPAT_FIXTURE).endsWith("tst_notify_order.qml");
        const QByteArray notifyBytes = "{\"settings\":{\"enabled\":true,\"rows\":[{\"compact\":"
                                       "false,\"art\":true}]},\"wide\":18446744073709551615}";
        if (notifyOrder)
            compat::put(dir.filePath("user.json"), notifyBytes);
        if (listOwnSave)
            compat::put(
                dir.filePath("user.json"),
                "{\"rows\":[{\"id\":\"x\",\"label\":\"original\"}],\"wide\":18446744073709551615}");
        const QByteArray numberBytes =
            "{\"rows\":[{\"id\":\"selected\",\"instanceSettings\":{\"threshold\":17,\"hide\":false}"
            ",\"opaque\":{\"keep\":true}}]}";
        if (overlayNumber) {
            compat::put(dir.filePath("base.json"), "{}");
            compat::put(dir.filePath("owner.json"),
                        numberLeg == "json" ? numberBytes : QByteArray("{}"));
        }
        if (draftRetry)
            compat::put(dir.filePath("user.json"),
                        "{\"settings\":{\"label\":\"before\"},\"opaque\":{\"keep\":true}}");
        const bool valueCopy = QString(COMPAT_FIXTURE).endsWith("tst_value_copy.qml");
        if (valueCopy) {
            const QByteArray record =
                "{\"id\":\"x\",\"label\":\"original\",\"knownNumber\":123,\"customField\":123,"
                "\"fraction\":1.25,\"wide\":18446744073709551615,\"overflow\":1e400,"
                "\"longDecimal\":1.2345678901234567890123456789,\"nested\":{\"__proto__\":{"
                "\"keep\":false}},\"items\":[{\"label\":\"child\",\"wide\":18446744073709551615}],"
                "\"constructor\":1e400,\"prototype\":\"kept\"}";
            compat::put(dir.filePath("user.json"),
                        "{\"rows\":[" + record + "],\"monitors\":{\"__proto__\":" + record +
                            ",\"other\":" + record + "},\"settings\":{\"object\":" + record + "}}");
        }
        if (liveOverlay) {
            compat::put(dir.filePath("base.json"), "{\"volume\":1}");
            compat::put(dir.filePath("main.json"),
                        "{\"rows\":[{\"id\":\"x\",\"sibling\":false,\"instanceSettings\":{"
                        "\"opaque\":{\"keep\":false}}}]}");
        }
        const bool pattern = QString(COMPAT_FIXTURE).endsWith("tst_string_pattern.qml");
        const bool sourceEvents = QString(COMPAT_FIXTURE).endsWith("tst_source_events.qml");
        const bool recordList = QString(COMPAT_FIXTURE).endsWith("tst_record_list_policy.qml");
        const bool unknownData = QString(COMPAT_FIXTURE).endsWith("tst_unknown_data.qml");
        const bool pendingRebase = QString(COMPAT_FIXTURE).endsWith("tst_pending_rebase.qml");
        const bool sharedParents = QString(COMPAT_FIXTURE).endsWith("tst_shared_parents.qml");
        if (sharedParents)
            QVERIFY(!QFileInfo::exists(dir.filePath("settings")));
        if (pendingRebase)
            compat::put(dir.filePath("user.json"), "{\"a\":1,\"b\":1}");
        if (unknownData)
            compat::put(
                dir.filePath("user.json"),
                "{\"record\":{\"id\":\"old\",\"extra\":5},\"open\":{\"opaque\":true},\"rogue\":4}");
        if (sourceEvents)
            QVERIFY(QDir().mkpath(dir.filePath("live")));
        if (retained)
            QVERIFY(QDir().mkpath(dir.filePath("blocked")));
        LockObstruction obstruction(
            dir.filePath(retained ? "blocked/user.json.lock" : "live/user.json.lock"));
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(COMPAT_FIXTURE));
        QVariantMap initial{{"directory", dir.path()}};
        if (listOwnSave)
            initial["scenario"] = crossSchedule;
        if (overlayNumber) {
            initial["leg"] = numberLeg;
            initial["mode"] = numberMode;
        }
        if (sourceEvents)
            initial["crossSchedule"] = crossSchedule;
        std::unique_ptr<QObject> root(component.createWithInitialProperties(initial));
        QVERIFY2(root, qPrintable(component.errorString()));
        QMap<QString, QPointer<ConfigOverlayEditor>> notificationOverlays;
        int overlayObservations = 0;
        QList<QMetaObject::Connection> notificationConnections;
        const auto disconnectNotifications = qScopeGuard([&] {
            for (const auto &connection : notificationConnections)
                QObject::disconnect(connection);
        });
        if (notifyOrder) {
            auto *placements = root->property("placements").value<ConfigQmlStore *>();
            QVERIFY(placements);
            const auto observe = [&] {
                for (auto it = notificationOverlays.cbegin(); it != notificationOverlays.cend();
                     ++it) {
                    auto *editor = it.value().data();
                    const auto payload = root->property(it.key().toUtf8());
                    auto *object = payload.value<QObject *>();
                    if (!object)
                        object = payload.value<QJSValue>().toQObject();
                    if (!editor || !object)
                        continue;
                    ++overlayObservations;
                    const auto rows =
                        engine.toScriptValue(root->property((it.key() + "Rows").toUtf8()));
                    if (!editor->ready()) {
                        QVERIFY(!root->property((it.key() + "Enabled").toUtf8()).isValid());
                        QCOMPARE(rows.property("length").toInt(), 0);
                        continue;
                    }
                    auto *values = object->property("values").value<QObject *>();
                    QVERIFY(values);
                    QCOMPARE(values->property("enabled").toBool(),
                             editor->value({"enabled"}).toBool());
                    QCOMPARE(root->property((it.key() + "Enabled").toUtf8()).toBool(),
                             editor->value({"enabled"}).toBool());
                    const auto expected = editor->value({"rows"}).toList();
                    QCOMPARE(rows.property(0).property("compact").toBool(),
                             expected[0].toMap()["compact"].toBool());
                    QCOMPARE(rows.property(0).property("art").toBool(),
                             expected[0].toMap()["art"].toBool());
                }
            };
            notificationConnections.append(connect(
                placements->core(), &ConfigStore::overlayOpened, this,
                [&, observe](quint64, ConfigOverlayEditor *editor) {
                    const auto key = notificationOverlays.isEmpty()     ? QString("view")
                                     : notificationOverlays.size() == 1 ? QString("live")
                                                                        : QString("editor");
                    notificationOverlays.insert(key, editor);
                    notificationConnections.append(
                        connect(editor, &ConfigOverlayEditor::valuesChanged, root.get(), observe));
                    notificationConnections.append(
                        connect(editor, &ConfigOverlayEditor::stateChanged, root.get(), observe));
                    notificationConnections.append(
                        connect(editor, &ConfigOverlayEditor::completed, root.get(), observe));
                }));
            notificationConnections.append(
                connect(placements->core(), &ConfigStore::valuesChanged, root.get(), observe));
        }
        QTRY_VERIFY2_WITH_TIMEOUT(root->property("ready").toBool() ||
                                      !root->property("failure").toString().isEmpty(),
                                  qPrintable(root->property("startupError").toString()), 10000);
        QCOMPARE(root->property("failure").toString(), QString());
        if (retained)
            QVERIFY(obstruction.block()); // After Ready; startup remains a real successful load.
        QVERIFY(QMetaObject::invokeMethod(root.get(), "run"));
        if (draftRetry) {
            const auto restore = qScopeGuard([&] {
                QFile::setPermissions(dir.path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            });
            QTRY_VERIFY(root->property("action").toString() == "block" ||
                        !root->property("failure").toString().isEmpty());
            QCOMPARE(root->property("failure").toString(), QString());
            QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
            QVERIFY(QMetaObject::invokeMethod(root.get(), "faultReady"));
            QTRY_VERIFY(root->property("action").toString() == "repair" ||
                        !root->property("failure").toString().isEmpty());
            QVERIFY(QFile::setPermissions(dir.path(),
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            QCOMPARE(root->property("failure").toString(), QString());
            const auto before =
                ConfigJsonCodec().decode(compat::bytes(dir.filePath("user.json")), "user").raw;
            QCOMPARE(before.value("settings").toMap().value("label").toString(), QString("before"));
            QVERIFY(QMetaObject::invokeMethod(root.get(), "retryReady"));
        }
        if (draftPreview) {
            QTRY_VERIFY(root->property("action").toString() == "foreign" ||
                        !root->property("failure").toString().isEmpty());
            QCOMPARE(root->property("failure").toString(), QString());
            // The preview is not a store layer or pending persistence intent.
            QVERIFY(!QFileInfo::exists(dir.filePath("shared.json")));
            QSaveFile foreign(dir.filePath("shared.json"));
            QVERIFY(foreign.open(QIODevice::WriteOnly));
            QVERIFY(foreign.write("{\"settings\":{\"volume\":7,\"label\":\"foreign\"}}") > 0);
            QVERIFY(foreign.commit());
            QVERIFY(QMetaObject::invokeMethod(root.get(), "foreignWritten"));
            QTRY_VERIFY(root->property("action").toString() == "destroy" ||
                        !root->property("failure").toString().isEmpty());
            QCOMPARE(root->property("failure").toString(), QString());
            const auto value = root->property("victim");
            auto *victim = value.value<QObject *>();
            if (!victim)
                victim = value.value<QJSValue>().toQObject();
            QVERIFY(victim);
            delete victim; // Destroy the QML draft adapter and its owned native draft.
        }
        if (liveOverlay) {
            QTRY_VERIFY(root->property("action").toString() == "foreign" ||
                        !root->property("failure").toString().isEmpty());
            QCOMPARE(root->property("failure").toString(), QString());
            QSaveFile foreign(dir.filePath("base.json"));
            QVERIFY(foreign.open(QIODevice::WriteOnly));
            QVERIFY(foreign.write("{\"volume\":8}") > 0);
            QVERIFY(foreign.commit());
            QVERIFY(QMetaObject::invokeMethod(root.get(), "foreignWritten"));
        }
        if (pendingRebase) {
            // The local edit is queued before this nonparticipating writer, and
            // StartupSnapshot deliberately leaves discovery to locked preflight.
            QSaveFile foreign(dir.filePath("user.json"));
            QVERIFY(foreign.open(QIODevice::WriteOnly));
            const QByteArray bytes = "{\"a\":7,\"b\":4,\"opaque\":true}";
            QCOMPARE(foreign.write(bytes), bytes.size());
            QVERIFY(foreign.commit());
            QVERIFY(QMetaObject::invokeMethod(root.get(), "foreignWritten"));
        }
        if (sourceEvents) {
            QElapsedTimer deadline;
            deadline.start();
            auto replace = [&](const QString &file, const QByteArray &bytes) {
                QSaveFile output(dir.filePath(file));
                QVERIFY(output.open(QIODevice::WriteOnly));
                QCOMPARE(output.write(bytes), bytes.size());
                QVERIFY(output.commit());
            };
            while (!root->property("finished").toBool() &&
                   root->property("failure").toString().isEmpty() && deadline.elapsed() < 10000) {
                const auto action = root->property("action").toString();
                if (action == "echo") {
                    compat::put(dir.filePath("live/watch-hint"), "hint");
                    QTest::qWait(150);
                } else if (action == "equal")
                    replace("live/user.json", compat::bytes(dir.filePath("live/user.json")));
                else if (action == "cross") {
                    replace("live/user.json", "{\"n\":5}");
                    if (crossSchedule > 0)
                        QTest::qSleep(crossSchedule); // Worker watches run on their own thread.
                    else if (crossSchedule < 0) {
                        // A real accepted watch generation, not a timed coalescing assumption.
                        QTRY_VERIFY_WITH_TIMEOUT(
                            root->property("crossAccepted").toInt() == 1 ||
                                !root->property("failure").toString().isEmpty(),
                            5000);
                        QCOMPARE(root->property("failure").toString(), QString());
                        QCOMPARE(root->property("observed").toInt(), 5);
                        QCOMPARE(root->property("observedOther").toInt(), 0);
                    }
                    replace("section.json", "{\"n\":6}");
                } else if (action == "malformed")
                    replace("live/user.json", "stale malformed observation");
                else if (action == "repair")
                    replace("live/user.json", "{\"n\":7}");
                else if (action == "block")
                    QVERIFY(obstruction.block());
                else if (action == "unblock") {
                    QVERIFY(obstruction.restore());
                    replace("live/user.json", "{\"n\":8}");
                } else if (action == "fallback")
                    replace("plugin.json", "{\"a\":3}");
                else if (action == "empty")
                    replace("plugin.json", "{}");
                if (!action.isEmpty())
                    QVERIFY(QMetaObject::invokeMethod(root.get(), "actionDone",
                                                      Q_ARG(QVariant, action)));
                QTest::qWait(1);
            }
            QVERIFY(obstruction.restore());
        }
        if (retained) {
            QTRY_VERIFY_WITH_TIMEOUT(root->property("stopped").toBool(), 10000);
            QVERIFY(obstruction.restore());
            QVERIFY(QMetaObject::invokeMethod(root.get(), "retry"));
        }
        if (notifyOrder) {
            QTRY_VERIFY(root->property("stage").toString() == "before-apply" ||
                        !root->property("failure").toString().isEmpty());
            QCOMPARE(root->property("failure").toString(), QString());
            QCOMPARE(compat::bytes(dir.filePath("user.json")), notifyBytes);
            QVERIFY(QMetaObject::invokeMethod(root.get(), "startApply"));
        }
        QTRY_VERIFY2_WITH_TIMEOUT(root->property("finished").toBool() ||
                                      !root->property("failure").toString().isEmpty(),
                                  qPrintable(root->property("stage").toString()), 10000);
        QCOMPARE(root->property("failure").toString(), QString());
        QVERIFY(root->property("finished").toBool());
        if (notifyOrder) {
            const auto bytes = compat::bytes(dir.filePath("user.json"));
            QVERIFY(bytes.contains("18446744073709551615"));
            const auto settings =
                ConfigJsonCodec().decode(bytes, "user").raw.value("settings").toMap();
            QCOMPARE(settings["enabled"].toBool(), false);
            const auto row = settings["rows"].toList().at(0).toMap();
            QCOMPARE(row["compact"].toBool(), false);
            QCOMPARE(row["art"].toBool(), true);
            QVERIFY(overlayObservations >= 20);
        }
        if (listOwnSave) {
            const auto raw =
                ConfigJsonCodec().decode(compat::bytes(dir.filePath("user.json")), "user").raw;
            QVERIFY(raw.value("rows").toList().isEmpty());
            QCOMPARE(raw.value("wide").value<ConfigNumber>().token(),
                     QString("18446744073709551615"));
        }
        if (overlayNumber) {
            QCOMPARE(compat::bytes(dir.filePath("base.json")), QByteArray("{}"));
            QCOMPARE(compat::bytes(dir.filePath("owner.json")),
                     numberLeg == "json" ? numberBytes : QByteArray("{}"));
        }
        if (strictEmpty || overlay || pattern || recordList || unknownData || pendingRebase ||
            sharedParents || liveOverlay || draftPreview || draftRetry || valueCopy) {
            if (draftRetry) {
                const auto raw =
                    ConfigJsonCodec().decode(compat::bytes(dir.filePath("user.json")), "user").raw;
                QCOMPARE(raw.value("settings").toMap().value("label").toString(), QString("after"));
                QVERIFY(raw.value("opaque").toMap().value("keep").toBool());
            }
            if (valueCopy) {
                const auto raw =
                    ConfigJsonCodec().decode(compat::bytes(dir.filePath("user.json")), "user").raw;
                const auto monitors = raw.value("monitors").toMap();
                const auto changed = monitors.value("__proto__").toMap();
                QCOMPARE(changed.value("wide").value<ConfigNumber>().token(),
                         QString("18446744073709551615"));
                QCOMPARE(changed.value("overflow").value<ConfigNumber>().token(), QString("1e400"));
                QCOMPARE(changed.value("longDecimal").value<ConfigNumber>().token(),
                         QString("1.2345678901234567890123456789"));
                QCOMPARE(changed.value("constructor").value<ConfigNumber>().token(),
                         QString("1e400"));
                QCOMPARE(changed.value("prototype").toString(), QString("kept"));
                const auto items = changed.value("items").toList();
                QCOMPARE(items.size(), 2);
                QCOMPARE(items[0].toMap().value("label").toString(), QString("changed child"));
                QCOMPARE(items[0].toMap().value("wide").value<ConfigNumber>().token(),
                         QString("18446744073709551615"));
                QCOMPARE(monitors.value("other").toMap().value("label").toString(),
                         QString("original"));
                const auto owner = ConfigJsonCodec()
                                       .decode(compat::bytes(dir.filePath("owner.json")), "owner")
                                       .raw;
                const auto instance = owner.value("rows")
                                          .toList()[1]
                                          .toMap()
                                          .value("instanceSettings")
                                          .toMap()
                                          .value("object")
                                          .toMap();
                QCOMPARE(instance.value("wide").value<ConfigNumber>().token(),
                         QString("18446744073709551615"));
                QCOMPARE(instance.value("overflow").value<ConfigNumber>().token(),
                         QString("1e400"));
            }
            if (draftPreview) {
                const auto row = ConfigJsonCodec()
                                     .decode(compat::bytes(dir.filePath("owner.json")), "owner")
                                     .raw.value("rows")
                                     .toList()[0]
                                     .toMap();
                QVERIFY(!row.contains("instanceSettings"));
            }
            if (liveOverlay) {
                const auto row = ConfigJsonCodec()
                                     .decode(compat::bytes(dir.filePath("main.json")), "main")
                                     .raw.value("rows")
                                     .toList()[0]
                                     .toMap();
                QCOMPARE(row.value("sibling"), QVariant(false));
                const auto overrides = row.value("instanceSettings").toMap();
                QVERIFY(!overrides.contains("volume"));
                QCOMPARE(overrides.value("opaque").toMap().value("keep"), QVariant(false));
                QCOMPARE(
                    overrides.value("lastResult").toMap().value("n").value<ConfigNumber>().token(),
                    QString("39"));
            }
            if (sharedParents) {
                for (const auto &file : {"general", "bar", "wallpaper"})
                    QVERIFY(QFileInfo::exists(dir.filePath(QString("settings/%1.json").arg(file))));
                const auto read = [&](const QString &name) {
                    return ConfigJsonCodec()
                        .decode(compat::bytes(dir.filePath("settings/" + name + ".json")), name)
                        .raw;
                };
                QCOMPARE(read("general")
                             .value("scaleRatio")
                             .value<ConfigNumber>()
                             .real()
                             .value.toDouble(),
                         1.74);
                QCOMPARE(read("bar").value("position").toString(), QString("left"));
                QCOMPARE(read("wallpaper").value("directory").toString(),
                         QString("/owner-wallpapers"));
            }
            if (strictEmpty) {
                QVERIFY(!QFileInfo::exists(dir.filePath("user.json")));
                QVERIFY(QFileInfo::exists(dir.filePath("user.json.lock")));
            }
            QQmlEngine restartedEngine;
            restartedEngine.addImportPath(QML_IMPORT_ROOT);
            QQmlComponent restarted(&restartedEngine, QUrl::fromLocalFile(COMPAT_FIXTURE));
            std::unique_ptr<QObject> consumer(
                restarted.createWithInitialProperties({{"directory", dir.path()}}));
            QVERIFY2(consumer, qPrintable(restarted.errorString()));
            QTRY_VERIFY(consumer->property("ready").toBool());
            QVERIFY(QMetaObject::invokeMethod(consumer.get(), "runRestart"));
            QTRY_VERIFY(consumer->property("finished").toBool() ||
                        !consumer->property("failure").toString().isEmpty());
            QCOMPARE(consumer->property("failure").toString(), QString());
            QVERIFY(consumer->property("finished").toBool());
        }
        if (recordList) {
            ConfigJsonCodec codec;
            const auto raw = codec.decode(compat::bytes(dir.filePath("user.json")), "user").raw;
            QCOMPARE(raw.value("rows").toList().size(), 1);
            QCOMPARE(raw.value("rows").toList().first().toMap().value("id").toString(),
                     QString("hidden"));
            const auto destination = raw.value("destination").toList();
            QCOMPARE(destination.size(), 1);
            QVERIFY(destination.first().toMap().contains("deprecated"));
            for (const auto &name : {"identity-before.json", "identity-after.json"}) {
                const auto screens = codec.decode(compat::bytes(dir.filePath(name)), "user")
                                         .raw.value("screens")
                                         .toList();
                QCOMPARE(screens.size(), 2);
                QCOMPARE(screens[0].toMap().value("name").toString(), QString("DP-1"));
                QCOMPARE(screens[1].toMap().value("name").toString(), QString("new"));
                QCOMPARE(screens[0].toMap().value("position").toString(), QString("left"));
                const auto widgets = screens[0].toMap().value("widgets").toList();
                QCOMPARE(widgets.size(), 2);
                QVERIFY(widgets[0].toMap().value("opaque").toBool());
                QVERIFY(widgets[1].toMap().value("deprecated").toBool());
                QVERIFY(!widgets[1].toMap().contains("compact"));
            }
        }
        if (retained) {
            ConfigJsonCodec codec;
            const auto raw = codec.decode(compat::bytes(dir.filePath("blocked/user.json")), "user");
            QCOMPARE(ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)
                         .validate(raw.raw.value("choice"))
                         .value.toInt(),
                     1);
        }
        QCOMPARE(compat::bytes(dir.filePath("legacy.json")),
                 QByteArray("{\"oldCount\":7,\"a\":1,\"b\":2,\"wide\":18446744073709551615}"));
    }
};
QTEST_MAIN(CompatibilityQmlTest)
#include "test_compat_qml.moc"
