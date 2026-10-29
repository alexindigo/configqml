// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "compatfixture.h"
#include <QQmlEngine>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QDir>
#include <QSaveFile>
#include <QElapsedTimer>
using namespace configqml;
class CompatibilityQmlTest : public QObject {
    Q_OBJECT
private slots:
    void boundConsumer() {
        QTemporaryDir dir;
        compat::put(dir.filePath("legacy.json"),
                    "{\"oldCount\":7,\"a\":1,\"b\":2,\"wide\":18446744073709551615}");
        compat::put(dir.filePath("bad.json"), "malformed");
        const bool retained = QString(COMPAT_FIXTURE).endsWith("tst_retained_groups.qml");
        const bool strictEmpty = QString(COMPAT_FIXTURE).endsWith("tst_strict_empty.qml");
        const bool overlay = QString(COMPAT_FIXTURE).endsWith("tst_overlay_editor.qml");
        const bool pattern = QString(COMPAT_FIXTURE).endsWith("tst_string_pattern.qml");
        const bool sourceEvents = QString(COMPAT_FIXTURE).endsWith("tst_source_events.qml");
        if (sourceEvents)
            QVERIFY(QDir().mkpath(dir.filePath("live")));
        if (retained) {
            QVERIFY(QDir().mkpath(dir.filePath("blocked")));
            QVERIFY(QFile::setPermissions(dir.filePath("blocked"),
                                          QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        }
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(COMPAT_FIXTURE));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"directory", dir.path()}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY_WITH_TIMEOUT(root->property("ready").toBool() ||
                                     !root->property("failure").toString().isEmpty(),
                                 10000);
        QCOMPARE(root->property("failure").toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "run"));
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
                    replace("section.json", "{\"n\":6}");
                } else if (action == "malformed")
                    replace("live/user.json", "stale malformed observation");
                else if (action == "repair")
                    replace("live/user.json", "{\"n\":7}");
                else if (action == "block")
                    QVERIFY(QFile::setPermissions(dir.filePath("live"),
                                                  QFileDevice::ReadOwner | QFileDevice::ExeOwner));
                else if (action == "unblock") {
                    QVERIFY(QFile::setPermissions(dir.filePath("live"),
                                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                      QFileDevice::ExeOwner));
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
            QVERIFY(QFile::setPermissions(dir.filePath("live"), QFileDevice::ReadOwner |
                                                                    QFileDevice::WriteOwner |
                                                                    QFileDevice::ExeOwner));
        }
        if (retained) {
            QTRY_VERIFY_WITH_TIMEOUT(root->property("stopped").toBool(), 10000);
            QVERIFY(QFile::setPermissions(dir.filePath("blocked"), QFileDevice::ReadOwner |
                                                                       QFileDevice::WriteOwner |
                                                                       QFileDevice::ExeOwner));
            QVERIFY(QMetaObject::invokeMethod(root.get(), "retry"));
        }
        QTRY_VERIFY2_WITH_TIMEOUT(root->property("finished").toBool() ||
                                      !root->property("failure").toString().isEmpty(),
                                  qPrintable(root->property("stage").toString()), 10000);
        QCOMPARE(root->property("failure").toString(), QString());
        QVERIFY(root->property("finished").toBool());
        if (strictEmpty || overlay || pattern) {
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
            QCOMPARE(consumer->property("failure").toString(), QString());
            QVERIFY(consumer->property("finished").toBool());
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
