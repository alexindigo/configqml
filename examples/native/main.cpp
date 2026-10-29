// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configstore.h>
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

using namespace configqml;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("configqml-native-example");
    using D = ConfigDescriptor;
    ConfigStoreOptions options;
    options.schema =
        ConfigSchema(D::group({{"position", D::leaf(D::Type::String, QStringLiteral("bottom"))}}));
    options.layers = {{"user",
                       ConfigLayerKind::File,
                       ConfigAdmissionPolicy::FieldFallback,
                       {{"preferences", {}, {}}},
                       true}};
    options.autoSave = false;
    ConfigStore store(options);
    QObject::connect(&store, &ConfigStore::statusChanged, &app, [&] {
        if (store.status() == ConfigStore::Status::Error) {
            qCritical() << store.error();
            app.exit(1);
        } else if (store.status() == ConfigStore::Status::Ready) {
            const auto input = store.setUser({"position"}, QStringLiteral("top"));
            if (!input.accepted || store.value({"position"}).toString() != "top") {
                app.exit(2);
                return;
            }
            store.close(&app, [&](const ConfigResult &result) {
                if (result.status != "saved" ||
                    store.snapshot().values.value(ConfigPath{"position"}).toString() != "top") {
                    app.exit(3);
                    return;
                }
                qInfo() << "CONFIGQML_NATIVE_OK" << configqml::version();
                app.quit();
            });
        }
    });
    QTimer::singleShot(10000, &app, [&] { app.exit(4); });
    return app.exec();
}
