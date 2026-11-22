// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configdraft.h>
#include <configjsoncodec.h>
#include <configoverlayeditor.h>
#include <configreadprojection.h>
#include <configstorehandle.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

using namespace configqml;
using D = ConfigDescriptor;
namespace {
void check(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
void wait(QEventLoop &loop) {
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(10000);
    loop.exec();
}
void ready(ConfigStore &store) {
    if (store.status() == ConfigStore::Status::Loading) {
        QEventLoop loop;
        QObject::connect(&store, &ConfigStore::statusChanged, &loop, &QEventLoop::quit);
        wait(loop);
    }
    check(store.status() == ConfigStore::Status::Ready, "store not Ready");
}
ConfigResult request(ConfigStore &store, std::function<quint64()> start) {
    QEventLoop loop;
    ConfigResult result;
    quint64 id = 0;
    QObject::connect(&store, &ConfigStore::completed, &loop, [&](const ConfigResult &r) {
        if (r.requestId == id) {
            result = r;
            loop.quit();
        }
    });
    id = start();
    check(id != 0, "operation refused before queue");
    wait(loop);
    check(result.requestId == id && result.status != "failed", "operation failed/timed out");
    return result;
}
ConfigReadResult synchronize(ConfigReadProjection *projection, const ConfigObservation &after) {
    QEventLoop loop;
    ConfigReadResult result;
    bool signal = false;
    QObject::connect(projection, &ConfigReadProjection::completed, &loop,
                     [&](const ConfigReadResult &) { signal = true; });
    projection->synchronize(after, &loop, [&](const ConfigReadResult &r) {
        check(signal, "callback before read signal");
        result = r;
        loop.quit();
    });
    wait(loop);
    check(result.status() == "observed", "read was not observed");
    return result;
}
QByteArray bytes(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "file readback open");
    return file.readAll();
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--inert-daemon") {
        if (app.arguments().size() != 3)
            return 2;
        std::printf("DAEMON_ARGV %s\n", qPrintable(app.arguments()[2]));
        return 0;
    }
    try {
        QTemporaryDir directory;
        check(directory.isValid(), "fixture directory");
        const auto file = directory.filePath("base.json");
        const QByteArray original("{\"count\":4,\"hidden\":8,\"opaque\":true}");
        {
            QFile initial(file);
            check(initial.open(QIODevice::WriteOnly), "fixture open");
            check(initial.write(original) == original.size(), "fixture write");
        }
        ConfigStoreOptions baseOptions;
        baseOptions.autoSave = false;
        baseOptions.schema = ConfigSchema(
            D::group({{"settings", D::group({{"count", D::leaf(D::Type::SafeInteger, 3)},
                                             {"hidden", D::leaf(D::Type::SafeInteger, 2)}})}}));
        baseOptions.layers = {{"user",
                               ConfigLayerKind::File,
                               ConfigAdmissionPolicy::FieldFallback,
                               {{"base", {"settings"}, {}}},
                               true}};
        baseOptions.jsonFiles = {{"user", "base", file}};
        ConfigStore base(baseOptions);
        ready(base);
        auto *draft = base.draft({{"settings"}});
        check(draft->setValue({"settings", "count"}, 12).accepted, "wizard set");
        request(base, [&] { return draft->stageReset({{"settings"}}); });
        const auto state = draft->readProjection()->selectedState({"settings", "count"});
        check(state.pending() == ConfigSelectedState::Pending::Remove && state.targetPresent() &&
                  state.candidateAvailable() && state.value().toInt() == 3,
              "Reset selected facts");
        check(draft->readProjection()->selectedState({"settings", "hidden"}).value().toInt() == 2,
              "undisplayed Reset leaf");
        check(bytes(file) == original && base.inputs().isEmpty(), "Reset had effects");
        check(draft->discard() && bytes(file) == original, "Discard changed bytes");
        request(base, [&] { return draft->stageReset({{"settings"}}); });
        ConfigObservation committed;
        QObject::connect(draft->readProjection(), &ConfigReadProjection::operationCompleted, &base,
                         [&](const ConfigResult &r, const ConfigObservation &token) {
                             if (r.operation == "apply")
                                 committed = token;
                         });
        request(base, [&] { return draft->apply(); });
        delete draft;
        check(committed.valid(), "committed token lost with draft");
        const auto raw = ConfigJsonCodec().decode(bytes(file), "base").raw;
        check(!raw.contains("count") && !raw.contains("hidden") && raw.contains("opaque"),
              "Reset raw deletion/opaque preservation");
        ConfigStore cold(baseOptions);
        ready(cold);
        check(cold.value({"settings", "count"}).toInt() == 3, "independent cold reader");
        synchronize(base.scope({"settings"}, false)->readProjection(), committed);

        auto row = D(D::Type::Record);
        row.fields = {{"id", D::leaf(D::Type::String, QString())}};
        ConfigStoreOptions ownerOptions;
        ownerOptions.autoSave = false;
        ownerOptions.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        ownerOptions.layers = {
            {"user",
             ConfigLayerKind::File,
             ConfigAdmissionPolicy::FieldFallback,
             {{"owner",
               {},
               {{"rows", QVariantList{QVariantMap{
                             {"id", "x"}, {"instanceSettings", QVariantMap{{"count", 4}}}}}}}}},
             true}};
        ConfigStore owner(ownerOptions);
        ready(owner);
        ConfigOverlayOptions options;
        options.base = base.scope({"settings"}, false);
        options.ownerPath = {"rows"};
        options.ownerGeneration = owner.info({"rows"}).publicationGeneration;
        options.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
        options.identityPath = {{ConfigValueSelector::Kind::Key, "id", 0}};
        options.identityValue = "x";
        options.overrideKey = "instanceSettings";
        ConfigOverlayEditor *opened = nullptr;
        QObject::connect(&owner, &ConfigStore::overlayOpened, &owner,
                         [&](quint64, ConfigOverlayEditor *editor) { opened = editor; });
        options.mode = ConfigOverlayMode::Live;
        request(owner, [&] { return owner.openOverlay(options); });
        auto *runtime = opened;
        options.mode = ConfigOverlayMode::Editor;
        request(owner, [&] { return owner.openOverlay(options); });
        auto *writer = opened;
        check(runtime && writer && runtime != writer, "independent runtime/writer");
        QObject::connect(
            writer->readProjection(), &ConfigReadProjection::operationCompleted, &owner,
            [&](const ConfigResult &, const ConfigObservation &token) { committed = token; });
        check(writer->unset({"count"}), "stage sparse removal");
        request(owner, [&] { return writer->apply(); });
        delete writer;
        const auto acknowledged = synchronize(runtime->readProjection(), committed);
        const auto captured = acknowledged.snapshot();
        check(runtime->value({"count"}).toInt() == 3 &&
                  captured.selectedState({"count"}).value().toInt() == 3,
              "runtime acknowledgment");
        QProcess consumer;
        consumer.start(
            app.applicationFilePath(),
            {"--inert-daemon", QString::number(captured.selectedState({"count"}).value().toInt())});
        check(consumer.waitForFinished(10000) && consumer.exitCode() == 0 &&
                  consumer.readAllStandardOutput().trimmed() == "DAEMON_ARGV 3",
              "final argv");
        check(runtime->setValue({"count"}, 7).accepted, "later live input");
        check(captured.selectedState({"count"}).value().toInt() == 3, "snapshot mutated");
        std::puts("GENERIC_INSTALLED_NATIVE_OK argv=3");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "GENERIC_INSTALLED_NATIVE_FAILED %s\n", error.what());
        return 1;
    }
    return 0;
}
