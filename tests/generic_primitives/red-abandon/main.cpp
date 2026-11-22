// SPDX-License-Identifier: LGPL-3.0-or-later
// Red reproduction: after a failed admitted draft Apply, the user's Discard must
// return readers to the saved value. Released API offers no way to abandon the
// admitted change, so readers keep the discarded value.
#include <configdraft.h>
#include <configstore.h>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <sys/stat.h>

using namespace configqml;
using D = ConfigDescriptor;
namespace {
void put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("fixture write");
}
int nativeCase(const QString &directory) {
    const auto file = directory + "/native/user.json";
    QDir().mkpath(directory + "/native");
    put(file, "{\"settings\":{\"n\":9}}");
    ConfigStoreOptions options;
    options.autoSave = false;
    options.schema =
        ConfigSchema(D::group({{"settings", D::group({{"n", D::leaf(D::Type::SafeInteger, 1)}})}}));
    options.layers = {{"user",
                       ConfigLayerKind::File,
                       ConfigAdmissionPolicy::FieldFallback,
                       {{"user", {}, {}}},
                       true}};
    options.jsonFiles = {{"user", "user", file}};
    options.fileFault = [](const QString &phase, const QString &) {
        return phase == "write" ? QString("red-write-fault") : QString();
    };
    ConfigStore store(options);
    QEventLoop loop;
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    QObject::connect(&store, &ConfigStore::statusChanged, &loop, &QEventLoop::quit);
    if (store.status() == ConfigStore::Status::Loading)
        loop.exec();
    auto *draft = store.draft({{"settings", "n"}});
    if (!draft->setValue({"settings", "n"}, 5).accepted)
        return 3;
    QString status;
    QEventLoop applied;
    draft->apply(nullptr, [&](const ConfigResult &result) {
        status = result.status;
        applied.quit();
    });
    QTimer::singleShot(10000, &applied, &QEventLoop::quit);
    applied.exec();
    if (status != "failed")
        return 4;
    draft->discard(); // The user discards the failed page.
    const auto reader = store.value({"settings", "n"}).toInt();
    std::printf("ABANDON_NATIVE saved=9 reader=%d\n", reader);
    return reader == 9 ? 0 : 1;
}
int qmlCase(const QString &directory) {
    QDir().mkpath(directory + "/qml");
    put(directory + "/qml/user.json", "{\"settings\":{\"n\":9}}");
    // A read-only directory makes the atomic replacement fail after admission.
    if (::chmod(QFile::encodeName(directory + "/qml").constData(), 0555) != 0)
        return 5;
    QQmlEngine engine;
    engine.addImportPath(RED_IMPORT_ROOT);
    QQmlComponent component(&engine, QUrl::fromLocalFile(RED_FIXTURE));
    std::unique_ptr<QObject> root(
        component.createWithInitialProperties({{"directory", directory + "/qml"}}));
    if (!root) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 6;
    }
    QEventLoop loop;
    QObject::connect(root.get(), SIGNAL(finishedChanged()), &loop, SLOT(quit()));
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    if (!root->property("finished").toBool())
        loop.exec();
    ::chmod(QFile::encodeName(directory + "/qml").constData(), 0755);
    if (!root->property("failure").toString().isEmpty()) {
        std::fprintf(stderr, "fixture: %s\n", qPrintable(root->property("failure").toString()));
        return 7;
    }
    const auto reader = root->property("reader").toInt();
    std::printf("ABANDON_QML saved=9 reader=%d\n", reader);
    return reader == 9 ? 0 : 1;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 2;
    const int native = nativeCase(directory.path());
    const int qml = qmlCase(directory.path());
    std::printf("ABANDON_RED native=%d qml=%d\n", native, qml);
    return native || qml ? 1 : 0;
}
