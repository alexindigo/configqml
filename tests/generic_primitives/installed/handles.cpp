// SPDX-License-Identifier: LGPL-3.0-or-later
// Core-only installed consumer of generated handles and composed replies.
#include <confighandle.h>
#include <configstore.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace configqml;
using D = ConfigDescriptor;
namespace {
void check(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
std::unique_ptr<ConfigPendingReply> settle(ConfigPendingReply *reply) {
    std::unique_ptr<ConfigPendingReply> owned(reply);
    check(reply, "null reply");
    if (!reply->isFinished()) {
        QEventLoop loop;
        QObject::connect(reply, &ConfigPendingReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    check(reply->isFinished(), "reply timeout");
    return owned;
}
std::unique_ptr<ConfigPendingReply> ok(ConfigPendingReply *reply, const char *reason) {
    auto owned = settle(reply);
    if (owned->isError())
        std::fprintf(stderr, "%s: %s/%s\n", reason, qPrintable(owned->error().phase()),
                     qPrintable(owned->error().code()));
    check(!owned->isError(), reason);
    return owned;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        check(directory.isValid(), "directory");
        const auto file = directory.filePath("user.json");
        {
            QFile seed(file);
            check(seed.open(QIODevice::WriteOnly) && seed.write("{}") == 2, "seed");
        }
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group(
            {{"plugins",
              D::group({{"clock", D::group({{"maxVisible", D::leaf(D::Type::Integer, 2)}})}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        options.jsonFiles = {{"user", "user", file}};
        ConfigStore store(options);
        if (store.status() == ConfigStore::Status::Loading) {
            QEventLoop loop;
            QObject::connect(&store, &ConfigStore::statusChanged, &loop, &QEventLoop::quit);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        check(store.status() == ConfigStore::Status::Ready, "store not Ready");
        auto *clock = store.root()->scope({"plugins", "clock"});
        auto *field = clock->scope({"maxVisible"});
        check(field->isField() && field->value().toInt() == 2, "generated field");
        int notified = 0;
        QObject::connect(field, &ConfigHandle::valueChanged, [&] { ++notified; });
        auto opened = ok(clock->edit(), "edit");
        std::unique_ptr<ConfigHandle> edit(opened->value().handle());
        check(bool(edit), "opened handle");
        edit->setParent(nullptr);
        ok(edit->scope({"maxVisible"})->set(3), "staged set");
        check(field->value().toInt() == 2 && notified == 0, "staging is not live");
        ConfigHandleOptions observe;
        observe.observe = {clock};
        auto applied = ok(edit->apply(observe), "apply");
        check(applied->value().write().has_value(), "write facts");
        check(applied->value()
                      .observations()
                      .value(0)
                      .snapshot()
                      .selectedState({"maxVisible"})
                      .value()
                      .toInt() == 3,
              "observed snapshot");
        check(field->value().toInt() == 3 && notified == 1, "bound transition once");
        const auto generation = applied->value().generation();
        edit.reset(); // Closing the edit does not invalidate committed coverage.
        ok(clock->observe(generation), "observe after close");
        QFile written(file);
        check(written.open(QIODevice::ReadOnly) &&
                  QJsonDocument::fromJson(written.readAll()).object() ==
                      QJsonObject{
                          {"plugins", QJsonObject{{"clock", QJsonObject{{"maxVisible", 3}}}}}},
              "persisted file");
        std::puts("GENERIC_INSTALLED_HANDLES_NATIVE_OK maxVisible=3");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "GENERIC_INSTALLED_HANDLES_NATIVE_FAILED %s\n", error.what());
        return 1;
    }
}
