// SPDX-License-Identifier: LGPL-3.0-or-later
#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTimer>
#include <QFile>
#include <iostream>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 && argc != 4)
        return 1;
    QQmlEngine engine;
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QCoreApplication::quit);
    QObject::connect(&engine, &QQmlEngine::exit, &app, &QCoreApplication::exit);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (component.isError()) {
        std::cerr << component.errorString().toStdString();
        return 2;
    }
    const auto object =
        component.createWithInitialProperties({{"directory", QString::fromLocal8Bit(argv[2])}});
    if (!object) {
        std::cerr << component.errorString().toStdString();
        return 3;
    }
    QTimer::singleShot(15000, &app, [&] { app.exit(4); });
    QTimer workflow;
    bool started = false;
    if (argc == 4) {
        const bool cold = QString::fromLocal8Bit(argv[3]) == "cold";
        QObject::connect(&workflow, &QTimer::timeout, &app, [&, cold] {
            if (!object->property("failure").toString().isEmpty()) {
                app.exit(12);
                return;
            }
            if (object->property("finished").toBool()) {
                app.quit();
                return;
            }
            if (!started && object->property("ready").toBool()) {
                started = true;
                if (!QMetaObject::invokeMethod(object, cold ? "runRestart" : "run"))
                    app.exit(13);
            }
            if (object->property("action").toString() == "check-before-apply") {
                QFile file(QString::fromLocal8Bit(argv[2]) + "/user.json");
                if (!file.open(QIODevice::ReadOnly) || file.readAll() != "{\"n\":9}") {
                    app.exit(14);
                    return;
                }
                QMetaObject::invokeMethod(object, "checkedDisk");
            }
            const auto action = object->property("action").toString();
            if (action == "publish-foreign" || action == "restore-foreign") {
                QFile user(QString::fromLocal8Bit(argv[2]) + "/user.json");
                QFile legacy(QString::fromLocal8Bit(argv[2]) + "/legacy-reset.json");
                if (!user.open(QIODevice::ReadOnly) || user.readAll() != "{\"n\":9}" ||
                    !legacy.open(QIODevice::WriteOnly)) {
                    app.exit(15);
                    return;
                }
                const bool foreign = action == "publish-foreign";
                const QByteArray bytes =
                    foreign ? "{\"n\":17,\"other\":18}" : "{\"n\":7,\"other\":8}";
                if (legacy.write(bytes) != bytes.size()) {
                    app.exit(16);
                    return;
                }
                legacy.close();
                QMetaObject::invokeMethod(object, foreign ? "reloadForeign" : "reloadRestored");
            }
        });
        workflow.start(1);
    }
    const auto rc = app.exec();
    std::cout << object->property("observations").toString().toStdString();
    if (const auto failure = object->property("failure").toString(); !failure.isEmpty())
        std::cerr << failure.toStdString() << '\n';
    delete object;
    return rc;
}
