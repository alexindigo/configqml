// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configdraft.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <iostream>
using namespace configqml;
using D = ConfigDescriptor;
static bool wait(ConfigStore &store) {
    if (store.status() == ConfigStore::Status::Ready)
        return true;
    QEventLoop loop;
    QObject::connect(&store, &ConfigStore::statusChanged, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    return store.status() == ConfigStore::Status::Ready;
}
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
    if (!wait(store))
        return 2;
    if (mode == "cold") {
        if (store.value({"n"}).toInt() != 1)
            return 3;
        std::cout << "OLD_NATIVE_COLD: EXPLICIT_DEFAULT_STILL_PRESENT\n";
        return 0;
    }
    auto *draft = store.draft({{"n"}});
    if (!draft->valid() || draft->value({"n"}).toInt() != 9 ||
        !draft->setValue({"n"}, 1).accepted || draft->value({"n"}).toInt() != 1 ||
        store.value({"n"}).toInt() != 9)
        return 4;
    QFile before(path);
    if (!before.open(QIODevice::ReadOnly) || before.readAll() != "{\"n\":9}")
        return 5;
    before.close();
    if (!draft->discard() || draft->value({"n"}).toInt() != 9 ||
        !draft->setValue({"n"}, 1).accepted)
        return 6;
    QEventLoop loop;
    ConfigResult result;
    draft->apply(&loop, [&](const ConfigResult &r) {
        result = r;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    QFile after(path);
    if (result.status != "saved" || !after.open(QIODevice::ReadOnly))
        return 7;
    const auto decoded = decodeJsonValue(after.readAll());
    if (!decoded.ok() || !decoded.value.toMap().contains("n") || store.value({"n"}).toInt() != 1)
        return 8;
    std::cout << "OLD_NATIVE_BASELINE: DEFAULT_ASSIGNMENT_NOT_REMOVAL\n";
}
