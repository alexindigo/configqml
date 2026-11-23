// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configdraft.h>
#include <configjsoncodec.h>
#include <configoverlayeditor.h>
#include <configstorehandle.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void ready(ConfigStore &store) {
    if (store.status() != ConfigStore::Status::Loading)
        return;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&store, &ConfigStore::statusChanged, &loop, &QEventLoop::quit);
    deadline.start(10000);
    loop.exec();
    check(store.status() == ConfigStore::Status::Ready, "store Ready");
}
ConfigResult request(ConfigStore &store, const std::function<quint64()> &start) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    quint64 id = 0;
    std::optional<ConfigResult> result;
    const auto connection = QObject::connect(&store, &ConfigStore::completed, &loop,
                                             [&](const ConfigResult &completed) {
                                                 if (completed.requestId == id) {
                                                     result = completed;
                                                     loop.quit();
                                                 }
                                             });
    id = start();
    deadline.start(10000);
    loop.exec();
    QObject::disconnect(connection);
    check(bool(result), "ordered request completion");
    return *result;
}
QByteArray bytes(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "raw readback open");
    return file.readAll();
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        check(directory.isValid(), "private fixture directory");
        const auto file = directory.filePath("user.json");
        {
            QFile initial(file);
            check(initial.open(QIODevice::WriteOnly), "fixture file");
            const QByteArray content = "{\"settings\":{\"count\":4,\"other\":6},\"opaque\":false}";
            check(initial.write(content) == content.size(), "fixture bytes");
        }
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"settings", D::group({{"count", D::leaf(T::SafeInteger, 1)},
                                             {"other", D::leaf(T::SafeInteger, 2)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        options.jsonFiles = {{"user", "user", file}};
        ConfigStore store(options);
        ready(store);
        check(store.value({"settings", "count"}).toInt() == 4, "initial admitted value");
        auto *scope = store.scope({"settings"});
        check(scope && scope->value({"count"}).toInt() == 4, "old inline scoped getter");
        check(request(store, [&] { return scope->setUser({"count"}, 3).requestId; }).status ==
                  "accepted",
              "scoped write admission");
        check(request(store, [&] { return store.setUser({"settings", "other"}, 7).requestId; })
                      .status == "accepted",
              "sibling write admission");
        check(request(store, [&] { return store.saveSelected({{"settings", "count"}}); }).status ==
                  "saved",
              "selected save");
        auto raw = ConfigJsonCodec().decode(bytes(file), "user").raw;
        auto settings = raw.value("settings").toMap();
        check(D::leaf(T::SafeInteger, 0).validate(settings.value("count")).value.toInt() == 3 &&
                  D::leaf(T::SafeInteger, 0).validate(settings.value("other")).value.toInt() == 6 &&
                  raw.contains("opaque"),
              "selected save preserves pending sibling and opaque raw");
        auto *draft = store.draft({{"settings", "count"}});
        check(draft && draft->setValue({"settings", "count"}, 8).accepted, "frozen draft set");
        check(draft->value({"settings", "count"}).toInt() == 8 &&
                  store.value({"settings", "count"}).toInt() == 3,
              "unapplied isolation");
        check(request(store, [&] { return draft->stageReset({{"settings", "count"}}); }).status ==
                  "staged",
              "staged Reset");
        check(draft->value({"settings", "count"}).toInt() == 1, "checked removal candidate");
        check(draft->discard(), "Discard");
        check(draft->value({"settings", "count"}).toInt() == 3, "Discard rebases");
        check(request(store, [&] { return draft->stageReset({{"settings", "count"}}); }).status ==
                  "staged",
              "second staged Reset");
        check(request(store, [&] { return draft->apply(); }).status == "saved", "Reset Apply");
        raw = ConfigJsonCodec().decode(bytes(file), "user").raw;
        check(!raw.value("settings").toMap().contains("count") && raw.contains("opaque"),
              "actual raw deletion");
        check(D::leaf(T::SafeInteger, 0)
                      .validate(raw.value("settings").toMap().value("other"))
                      .value.toInt() == 6,
              "removal Apply keeps selected publication");
        ConfigStore cold(options);
        ready(cold);
        check(cold.value({"settings", "count"}).toInt() == 1 &&
                  cold.value({"settings", "other"}).toInt() == 6,
              "independent cold reader");
        const auto before = store.info({"settings", "count"}).intentRevision;
        check(request(store, [&] { return store.setUser({"settings", "count"}, 1).requestId; })
                      .status == "accepted",
              "explicit equal write");
        check(store.info({"settings", "count"}).intentRevision > before,
              "equal writes retain intent");
        check(request(store, [&] { return store.flush(); }).status == "saved", "full flush");
        QElapsedTimer clock;
        QList<qint64> spans;
        for (int sample = 0; sample < 21; ++sample) {
            clock.start();
            qint64 sum = 0;
            for (int i = 0; i < 10000; ++i)
                sum += store.presentation({"settings", "count"}).toInt();
            const auto nanos = clock.nsecsElapsed();
            check(sum == 10000, "cached getter result");
            spans.append(nanos);
        }
        QVariantList timings;
        for (const auto nanos : spans)
            timings.append(QString::number(nanos));
        const QVariantMap performance{{"span", "cached-presentation-10000"},
                                      {"nanoseconds", timings}};
        std::printf(
            "OLD_NATIVE_PERFORMANCE %s\n",
            QJsonDocument::fromVariant(performance).toJson(QJsonDocument::Compact).constData());
        check(request(cold, [&] { return cold.close(); }).status == "nothing_to_save",
              "cold close");
        check(request(store, [&] { return store.close(); }).status == "nothing_to_save", "close");
        std::puts("OLD_NATIVE_OK");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "OLD_NATIVE_FAILED %s\n", error.what());
        return 1;
    }
}
