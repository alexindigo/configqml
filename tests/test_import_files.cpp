// SPDX-License-Identifier: LGPL-3.0-or-later
// Independent cold consumer: only QtQml, no library headers or configqml linkage.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>
#include <memory>
static bool await(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
static int run(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    for (int cycle = 0; cycle < 4; ++cycle) {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_FILE_FIXTURE));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"directory", dir.path()}, {"automatic", cycle == 2}}));
        if (!root) {
            std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
            return 1;
        }
        auto *store = root->property("store").value<QObject *>();
        if (!store || !await([&] { return store->property("ready").toBool(); }))
            return 2;
        if (!cycle) {
            if (!QMetaObject::invokeMethod(root.get(), "editAndClose"))
                return 3;
        } else if (cycle == 1) {
            if (root->property("count").toInt() != 2 ||
                root->property("wide").toString() != "18446744073709551615")
                return 4;
            QSaveFile edit(dir.filePath("bar.json"));
            const QByteArray bytes = "{\"items\":[\"external\"],\"wide\":18446744073709551614}";
            if (!edit.open(QIODevice::WriteOnly) || edit.write(bytes) != bytes.size() ||
                !edit.commit())
                return 5;
            if (!await([&] {
                    return root->property("count").toInt() == 1 &&
                           root->property("wide").toString() == "18446744073709551614";
                }))
                return 6;
            QMetaObject::invokeMethod(root.get(), "seal");
        } else {
            if (root->property("count").toInt() != (cycle == 2 ? 1 : 2))
                return 10;
            if (cycle == 2) {
                QMetaObject::invokeMethod(root.get(), "autoEdit");
                if (!await([&] {
                        const auto value = root->property("receipt");
                        const auto map = value.metaType() == QMetaType::fromType<QJSValue>()
                                             ? value.value<QJSValue>().toVariant().toMap()
                                             : value.toMap();
                        return map.value("status") == "saved" &&
                               map.value("operation") == "autosave";
                    }))
                    return 11;
            }
            QMetaObject::invokeMethod(root.get(), "seal");
        }
        if (!await([&] { return store->property("status").toInt() == 4; }))
            return 7;
        const auto receiptValue = root->property("receipt");
        const auto receipt = receiptValue.metaType() == QMetaType::fromType<QJSValue>()
                                 ? receiptValue.value<QJSValue>().toVariant().toMap()
                                 : receiptValue.toMap();
        if (receipt.value("status") != (cycle ? "nothing_to_save" : "saved"))
            return 8;
    }
    if (QFile::exists(dir.filePath("other.json")))
        return 9;
    std::puts("JSON import-only gate: two appends, exact uint64, close/save, cold restarts, live "
              "external edit and autoSave reached QML bindings; untouched shard absent");
    return 0;
}
int main(int argc, char **argv) {
    const auto status = run(argc, argv);
    if (status)
        std::fprintf(stderr, "JSON import-only gate failed at stage %d\n", status);
    return status;
}
