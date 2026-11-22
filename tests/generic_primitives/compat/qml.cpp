// SPDX-License-Identifier: LGPL-3.0-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

#ifndef CONFIGQML_CLIENT_MARKER
#define CONFIGQML_CLIENT_MARKER "OLD_QML_OK"
#endif

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 3)
        return 2;
    QTemporaryDir directory;
    if (!directory.isValid())
        return 3;
    QQmlEngine engine;
    // Never inherit a build-tree import path from a calling shell.
    engine.addImportPath(arguments[1]);
    QQmlComponent component(&engine, QUrl::fromLocalFile(arguments[2]));
    std::unique_ptr<QObject> root(
        component.createWithInitialProperties({{"directory", directory.path()}}));
    if (!root) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 4;
    }
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(root.get(), SIGNAL(finishedChanged()), &loop, SLOT(quit()));
    QObject::connect(root.get(), SIGNAL(failureChanged()), &loop, SLOT(quit()));
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(10000);
    if (!root->property("finished").toBool() && root->property("failure").toString().isEmpty())
        loop.exec();
    if (!root->property("finished").toBool() || !root->property("failure").toString().isEmpty()) {
        std::fprintf(stderr, "OLD_QML_FAILED %s\n",
                     qPrintable(root->property("failure").toString()));
        return 5;
    }
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly))
        return 6;
    for (const auto &line : maps.readAll().split('\n'))
        if (line.contains("libconfigqml"))
            std::printf("OLD_QML_LOADER %s\n", line.constData());
    std::puts(CONFIGQML_CLIENT_MARKER);
    return 0;
}
