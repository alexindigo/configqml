// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configdraft.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <iostream>
using namespace configqml;
using D = ConfigDescriptor;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3)
        return 1;
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const QString path = QString::fromLocal8Bit(argv[2]);
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group({{"n", D::leaf(D::Type::SafeInteger, 1)}}));
    o.layers = {{"legacy",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"legacy", {}, {{"n", 7}}}},
                 false},
                {"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"user", {}, {}}},
                 true}};
    o.jsonFiles = {{"user", "user", path}};
    ConfigStore store(o);
    QEventLoop ready;
    QObject::connect(&store, &ConfigStore::statusChanged, &ready, &QEventLoop::quit);
    QTimer::singleShot(10000, &ready, &QEventLoop::quit);
    ready.exec();
    if (store.status() != ConfigStore::Status::Ready)
        return 2;
    if (mode == "cold") {
        if (store.value({"n"}).toInt() != 7)
            return 3;
        std::cout << "NEW_NATIVE_COLD: LEGACY_7\n";
        return 0;
    }
    auto *draft = store.draft({{"n"}});
    auto await = [&](bool apply) {
        QEventLoop loop;
        ConfigResult result;
        auto callback = [&](const ConfigResult &r) {
            result = r;
            loop.quit();
        };
        if (apply)
            draft->apply(&loop, callback);
        else
            draft->stageReset({{"n"}}, &loop, callback);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        return result;
    };
    if (!draft->valid() || draft->value({"n"}).toInt() != 9 || await(false).status != "staged" ||
        draft->value({"n"}).toInt() != 7 || draft->previewValue({"n"}).toInt() != 7 ||
        store.value({"n"}).toInt() != 9 || !store.inputs().isEmpty())
        return 4;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != "{\"n\":9}")
        return 5;
    file.close();
    if (!draft->discard() || draft->value({"n"}).toInt() != 9 || await(false).status != "staged" ||
        await(true).status != "saved" || draft->dirty())
        return 6;
    if (!file.open(QIODevice::ReadOnly) ||
        decodeJsonValue(file.readAll()).value.toMap().contains("n") ||
        store.value({"n"}).toInt() != 7)
        return 7;
    std::cout << "NEW_NATIVE_INSTALLED: STAGED_DISCARD_APPLY_RAW_REMOVAL\n";
}
