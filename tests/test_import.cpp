// SPDX-License-Identifier: LGPL-3.0-or-later
// Independent consumer: links QtQml only, no configqml headers or link target.
#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QJSValue>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <functional>
#include <memory>
static bool await(const std::function<bool()> &ready) {
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
int main(int argc, char **argv) {
    auto fail = [](int code, const char *message) {
        std::fprintf(stderr, "import-only stage %d: %s\n", code, message);
        return code;
    };
    QCoreApplication app(argc, argv);
    QQmlEngine engine;
    engine.addImportPath(QML_IMPORT_ROOT);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QML_FIXTURE));
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    auto *store = root->property("store").value<QObject *>();
    if (!store || !await([&] { return store->property("ready").toBool(); }))
        return fail(2, "not ready");
    QMetaObject::invokeMethod(root.get(), "appendBoth");
    if (root->property("count").toInt() != 5)
        return fail(3, "bound list consumer did not observe both appends");
    QVariant ignored;
    QMetaObject::invokeMethod(
        root.get(), "run", Q_RETURN_ARG(QVariant, ignored),
        Q_ARG(QString, QString("cfg.bar.wide='-9007199254740993';store.close(function(r){"
                               "closedWide=cfg.bar.wide;callbackResults.push(r)})")));
    if (!await([&] { return store->property("status").toInt() == 4; }))
        return fail(4, "close did not settle");
    auto results = root->property("callbackResults").value<QJSValue>().toVariant().toList();
    if (results.size() != 1 || results.first().toMap().value("status") != "saved" ||
        results.first().toMap().value("inputPrefix") != "3")
        return fail(5, "close receipt or exact prefix mismatch");
    if (root->property("closedWide").toString() != "-9007199254740993")
        return fail(6, "close callback did not read the exact authoritative value");
    std::puts("Import-only independent consumer: bound append count=5, exact wide read, close "
              "saved prefix=3");
    return 0;
}
