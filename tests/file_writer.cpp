// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fileoptions.h"
#include "storeprobe.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
using namespace configqml;
static int unsettledProbe(QCoreApplication &app, const QStringList &args) {
    auto options = fileOptions(args[1]);
    options.jsonFiles.clear(); // Deterministically held memory publication, no wall-clock IO stall.
    options.autoSave = true;
    options.autoSaveInterval = 60000;
    options.storeId = "unsettled-test";
    auto *store = new ConfigStore(options);
    QElapsedTimer timer;
    timer.start();
    while (store->status() == ConfigStore::Status::Loading && timer.elapsed() < 5000)
        app.processEvents();
    if (store->status() != ConfigStore::Status::Ready)
        return 41;
    ConfigStoreTestAccess::run(*store, [](auto &worker) {
        worker.scheduleBackend({true, ConfigMemoryBackend::Fault::None});
    });
    store->setUser({args[2]}, args[3].toLongLong());
    store->close();
    delete store;
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
static int lifetimeProbe(QCoreApplication &app, const QStringList &args) {
    struct Evidence {
        QList<QPointer<ConfigStore>> stores;
        int deletedStatus = -1;
        int callbacks = 0;
        int heartbeats = 0;
    };
    const auto state = std::make_shared<Evidence>();
    const auto mode = args[4];
    const bool returning = mode.startsWith("lifetime-return");
    const bool inDrain = mode == "lifetime-drain-delete";
    const bool multiple = mode == "lifetime-multiple";
    const int count = multiple ? 3 : 1;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [] { std::cout << "QUIT_SIGNAL" << std::endl; });
    for (int i = 0; i < count; ++i) {
        const auto path = i ? args[1] + QString(".%1").arg(i) : args[1];
        auto options = fileOptions(path);
        options.autoSave = !mode.startsWith("lifetime-delete-manual") && !(multiple && i == 2);
        options.autoSaveInterval = 60000; // Exit must not rely on the autosave grace period.
        options.fileFault = [state, mode, returning, inDrain, i](const auto &phase, const auto &) {
            if (phase != "write")
                return QString();
            if (!returning && i == 0) {
                auto *application = QCoreApplication::instance();
                QMetaObject::invokeMethod(
                    application,
                    [application, state, inDrain] {
                        if (inDrain)
                            QTimer::singleShot(30, application, [state] {
                                auto *store = state->stores[0].data();
                                if (store) {
                                    state->deletedStatus = int(store->status());
                                    std::cout << "DELETE_IN_DRAIN status=" << state->deletedStatus
                                              << std::endl;
                                    delete store;
                                }
                            });
                        application->quit();
                    },
                    Qt::QueuedConnection);
            }
            QThread::msleep(300);
            return QString();
        };
        auto *store = new ConfigStore(options, mode == "lifetime-return-live" ? &app : nullptr);
        state->stores.append(store);
        QElapsedTimer readiness;
        readiness.start();
        while (store->status() == ConfigStore::Status::Loading && readiness.elapsed() < 5000)
            app.processEvents();
        if (store->status() != ConfigStore::Status::Ready)
            return 40;
        QObject::connect(store, &ConfigStore::completed, &app, [state](const auto &result) {
            ++state->callbacks;
            std::cout << "RECEIPT " << result.operation.toStdString() << " "
                      << result.status.toStdString() << " prefix=" << result.inputPrefix
                      << std::endl;
        });
    }
    // All consumers are Ready before any worker can request quit.
    for (int i = 0; i < count; ++i) {
        auto *store = state->stores[i].data();
        store->setUser({args[2]}, args[3].toLongLong() + i);
        if (inDrain || mode == "lifetime-delete-manual-save")
            store->save();
        else if (mode == "lifetime-delete-manual-flush" || (multiple && i == 2))
            store->flush();
        else if (mode == "lifetime-delete-manual-close")
            store->close();
        if (mode != "lifetime-return-live" && !inDrain && !(multiple && i == 1))
            delete store;
    }
    if (returning) {
        std::cout << "RETURN_WITHOUT_EXEC" << std::endl;
        return 0; // qAddPostRoutine, not an exec()/aboutToQuit event, owns settlement.
    }
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [state] { ++state->heartbeats; });
    heartbeat.start(5);
    const auto result = app.exec();
    for (const auto &store : state->stores)
        delete store.data();
    std::cout << "HEARTBEATS " << state->heartbeats << std::endl;
    return result;
}
static int shutdownProbe(QCoreApplication &app, const QStringList &args) {
    const auto mode = args[4];
    auto options = fileOptions(args[1]);
    options.autoSave = mode != "shutdown-closing-manual";
    options.autoSaveInterval = 60000;
    ConfigStore *store = nullptr;
    const bool destroy = mode == "shutdown-destroy-closing";
    options.fileFault = [&](const QString &phase, const QString &) {
        if (phase != "write")
            return QString();
        if (mode != "shutdown-closed")
            QMetaObject::invokeMethod(
                &app,
                [&] {
                    if (destroy) {
                        delete store;
                        store = nullptr;
                    }
                    app.quit();
                },
                Qt::QueuedConnection);
        QThread::msleep(200);
        return mode == "shutdown-closing-fault" ? QString("shutdown-write-fault") : QString();
    };
    store = new ConfigStore(options);
    QElapsedTimer elapsed;
    elapsed.start();
    while (store->status() == ConfigStore::Status::Loading && elapsed.elapsed() < 5000)
        app.processEvents();
    if (store->status() != ConfigStore::Status::Ready)
        return 30;
    int heartbeats = 0, closes = 0;
    QString status, error, saved;
    quint64 prefix = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
    heartbeat.start(5);
    QObject::connect(store, &ConfigStore::completed, &app, [&](const auto &result) {
        if (result.operation == "save")
            saved = result.status;
        if (result.operation == "close") {
            ++closes;
            status = result.status;
            error = result.error.code;
            prefix = result.inputPrefix;
            if (mode == "shutdown-closed")
                QTimer::singleShot(0, &app, &QCoreApplication::quit);
        }
    });
    elapsed.restart();
    store->setUser({args[2]}, args[3].toLongLong());
    if (mode == "shutdown-ready")
        store->save();
    else
        store->close();
    const auto exit = app.exec();
    const auto duration = elapsed.elapsed();
    delete store;
    std::cout << QJsonDocument(QJsonObject{{"closes", closes},
                                           {"status", status},
                                           {"error", error},
                                           {"saved", saved},
                                           {"prefix", QString::number(prefix)},
                                           {"heartbeats", heartbeats},
                                           {"elapsedMs", duration}})
                     .toJson(QJsonDocument::Compact)
                     .toStdString()
              << std::endl;
    return exit;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 5)
        return 20;
    if (args[4] == "unsettled")
        return unsettledProbe(app, args);
    if (args[4].startsWith("lifetime-"))
        return lifetimeProbe(app, args);
    if (args[4].startsWith("shutdown-"))
        return shutdownProbe(app, args);
    auto o = fileOptions(args[1]);
    o.autoSave = args[4] == "quit";
    o.autoSaveInterval = 60000;
    ConfigStore store(o);
    QElapsedTimer deadline;
    deadline.start();
    while (store.status() == ConfigStore::Status::Loading && deadline.elapsed() < 5000)
        app.processEvents();
    if (store.status() != ConfigStore::Status::Ready)
        return 21;
    std::cout << "READY" << std::endl;
    if (!o.autoSave) {
        std::string go;
        std::getline(std::cin, go);
        if (go != "GO")
            return 22;
    }
    store.setUser({args[2]}, args[3].toLongLong());
    if (o.autoSave) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        return app.exec();
    }
    bool settled = false;
    QString status;
    if (args[4] == "manual-held-teardown")
        ConfigStoreTestAccess::run(store, [](auto &w) { w.holdReplies(true); });
    store.close(&app, [&](const auto &r) {
        status = r.status;
        std::cout << QJsonDocument(QJsonObject{{"status", r.status},
                                               {"error", r.error.code},
                                               {"phase", r.error.operation},
                                               {"file", r.error.file}})
                         .toJson(QJsonDocument::Compact)
                         .toStdString()
                  << std::endl;
        settled = true;
    });
    if (args[4] == "manual-held-teardown")
        ConfigStoreTestAccess::run(store, [](auto &w) {
            w.holdReplies(false);
            QThread::msleep(500); // Close receipt precedes actual worker exit in the old owner.
        });
    deadline.restart();
    while (!settled && deadline.elapsed() < 5000)
        app.processEvents();
    std::cout << status.toStdString() << std::endl;
    return !settled ? 23 : status == "saved" ? 0 : status == "failed" ? 3 : 24;
}
