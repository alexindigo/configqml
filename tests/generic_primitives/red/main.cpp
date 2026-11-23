// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configworker_p.h"
#include "configshutdown_p.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

using namespace configqml;

// Baseline-only scheduling adapter. Uses v0.5.3's existing test friendship;
// never compiles production copies or changes the library under investigation.
namespace configqml {
class ConfigStoreTestAccess {
public:
    static void holdReader(ConfigStore &store, const QString &id, QObject *context,
                           QList<ConfigWorkerPacket> &held, const std::function<void()> &onHeld) {
        QObject::disconnect(store.m_owner.get(), &ConfigWorkerOwner::packetReady, &store,
                            &ConfigStore::receive);
        QObject::connect(store.m_owner.get(), &ConfigWorkerOwner::packetReady, context,
                         [&store, id, &held, &onHeld](const ConfigWorkerPacket &packet) {
                             auto delivered = packet;
                             delivered.overlays.clear();
                             for (const auto &presentation : packet.overlays) {
                                 if (presentation.editorId != id || !presentation.refreshed) {
                                     delivered.overlays.append(presentation);
                                     continue;
                                 }
                                 ConfigWorkerPacket retained;
                                 retained.snapshot.topology = packet.snapshot.topology;
                                 retained.snapshot.generation = packet.snapshot.generation;
                                 retained.overlays = {presentation};
                                 held.append(std::move(retained));
                                 if (onHeld)
                                     onHeld();
                             }
                             store.receive(delivered);
                         });
    }
    static void releaseReader(ConfigStore &store, QList<ConfigWorkerPacket> &held) {
        const auto retained = std::move(held);
        held.clear();
        for (const auto &packet : retained)
            store.receive(packet);
    }
    static ConfigOverlayEditor *reader(ConfigStore &store, const QString &id) {
        return store.m_overlays.value(id);
    }
};
} // namespace configqml

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture open");
    require(file.write(bytes) == bytes.size(), "fixture write");
}
void awaitSignal(QObject &object, const char *signal, const std::function<void()> &start) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool expired = false;
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        expired = true;
        loop.quit();
    });
    const auto connection = QObject::connect(&object, signal, &loop, SLOT(quit()));
    require(bool(connection), "fixture signal");
    deadline.start(10000);
    start();
    loop.exec();
    QObject::disconnect(connection);
    require(!expired, "fixture deadline (not the required behavioral red)");
}
} // namespace

int run(QCoreApplication &app) {
    if (app.arguments().value(1) == "--inert-daemon") {
        require(app.arguments().size() == 3, "inert daemon argv");
        std::printf("DAEMON_ARGV %s\n", qPrintable(app.arguments()[2]));
        return 0;
    }
    QTemporaryDir directory;
    require(directory.isValid(), "disk fixture directory");
    put(directory.filePath("base.json"), "{\"count\":1}");
    put(directory.filePath("owner.json"),
        "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"count\":4}}]}");
    QQmlEngine engine;
    engine.addImportPath(RED_IMPORT_ROOT);
    QQmlComponent component(&engine, QUrl::fromLocalFile(RED_FIXTURE));
    std::unique_ptr<QObject> root(
        component.createWithInitialProperties({{"directory", directory.path()}}));
    if (!root)
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
    require(bool(root), "QML construction");
    if (!root->property("ready").toBool())
        awaitSignal(*root, SIGNAL(readyChanged()), [] {});
    require(root->property("ready").toBool(), "stores ready");
    awaitSignal(*root, SIGNAL(setupFinished()), [&] {
        require(QMetaObject::invokeMethod(root.get(), "setup"), "setup invocation");
    });
    require(root->property("failure").toString().isEmpty(), "overlay setup");
    auto *owner = root->property("owner").value<ConfigQmlStore *>();
    auto *runtime = engine.toScriptValue(root->property("runtime")).toQObject();
    require(owner && runtime, "separate runtime handle");
    const auto id = runtime->property("editorId").toString();
    auto *nativeReader = ConfigStoreTestAccess::reader(*owner->core(), id);
    require(nativeReader && nativeReader->value({"count"}).toInt() == 4, "native runtime starts 4");
    require(root->property("boundCount").toInt() == 4, "bound runtime starts 4");
    QList<ConfigWorkerPacket> held;
    std::function<void()> onHeld;
    QObject scheduler;
    ConfigStoreTestAccess::holdReader(*owner->core(), id, &scheduler, held, onHeld);
    awaitSignal(*root, SIGNAL(writerFinished()), [&] {
        require(QMetaObject::invokeMethod(root.get(), "apply"), "Apply invocation");
    });
    require(root->property("writeStatus").toString() == "saved" &&
                root->property("refreshState").toString() == "current",
            "writer must have real saved/current completion");
    const int writer = root->property("writerAtCompletion").toInt();
    const int reader = root->property("readerAtCompletion").toInt();
    const int binding = root->property("bindingAtCompletion").toInt();
    if (held.isEmpty()) {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        onHeld = [&] { loop.quit(); };
        deadline.start(10000);
        loop.exec();
        onHeld = {};
    }
    require(!held.isEmpty(), "runtime capture held independently of writer confirmation");
    require(writer == 3 && reader == nativeReader->value({"count"}).toInt() && reader == binding,
            "writer/native/getter/binding observations agree");
    QProcess consumer;
    consumer.start(QCoreApplication::applicationFilePath(),
                   {"--inert-daemon", QString::number(reader)});
    require(consumer.waitForFinished(10000) && consumer.exitStatus() == QProcess::NormalExit &&
                consumer.exitCode() == 0,
            "inert daemon completion");
    const auto output = consumer.readAllStandardOutput().trimmed();
    require(output == "DAEMON_ARGV " + QByteArray::number(reader),
            "independent final argv readback");
    ConfigStoreTestAccess::releaseReader(*owner->core(), held);
    require(nativeReader->value({"count"}).toInt() == 3 &&
                root->property("boundCount").toInt() == 3,
            "releasing the exact held capture updates native and retained binding");
    if (writer != 3 || reader != 3 || binding != 3 || output != "DAEMON_ARGV 3") {
        std::fprintf(stderr, "GENERIC_STALE_READ writer=%d reader=%d argv=%d\n", writer, reader,
                     reader);
        std::fprintf(stderr,
                     "GENERIC_STALE_BINDING binding=%d released_native=3 released_binding=3\n",
                     binding);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        return run(app);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "GENERIC_RED_SETUP_FAILURE %s\n", error.what());
        return 2;
    }
}
