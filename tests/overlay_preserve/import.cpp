// SPDX-License-Identifier: LGPL-3.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>
#include <memory>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4)
        return 2;
    const auto directory = QString::fromLocal8Bit(argv[2]);
    const bool cold = QByteArray(argv[3]) == "cold";
    if (!QDir().mkpath(directory))
        return 3;
    if (!cold) {
        for (const auto &entry : QList<QPair<QString, QByteArray>>{
                 {"base.json", "{\"count\":0,\"flag\":false,\"pages\":[]}"},
                 {"owner.json", "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"count\":0,"
                                "\"pages\":[],\"opaque\":{\"wide\":18446744073709551615}}}]}"}}) {
            QFile file(directory + '/' + entry.first);
            if (!file.open(QIODevice::WriteOnly) || file.write(entry.second) != entry.second.size())
                return 4;
        }
    }
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (!component.isReady()) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 5;
    }
    const std::unique_ptr<QObject> root(
        component.createWithInitialProperties({{"directory", directory}, {"cold", cold}}));
    if (!root)
        return 6;
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (root->property("finished").toBool() || !root->property("failure").toString().isEmpty())
            loop.quit();
    });
    timer.start();
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!root->property("finished").toBool() || !root->property("failure").toString().isEmpty()) {
        std::fprintf(stderr, "QML consumer failed: %s\n",
                     qPrintable(root->property("failure").toString()));
        return 7;
    }
    QFile file(directory + "/owner.json");
    if (!file.open(QIODevice::ReadOnly))
        return 8;
    const auto bytes = file.readAll();
    const auto stored = QJsonDocument::fromJson(bytes)
                            .object()
                            .value("rows")
                            .toArray()[0]
                            .toObject()
                            .value("instanceSettings")
                            .toObject();
    if (!stored.contains("count") || stored.value("count").toInt(-1) != 0 ||
        !stored.contains("pages") || !stored.value("pages").toArray().isEmpty() ||
        !stored.value("opaque").toObject().value("wide").isDouble() ||
        !QRegularExpression(QStringLiteral("\"wide\"\\s*:\\s*18446744073709551615(?=\\s*[,}])"))
             .match(QString::fromUtf8(bytes))
             .hasMatch())
        return 9;
    std::puts(cold ? "INSTALLED_QML_COLD: PASS" : "INSTALLED_QML_PRESERVATION: PASS");
}
