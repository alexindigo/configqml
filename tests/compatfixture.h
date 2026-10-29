// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configstore.h"
#include <QFile>
#include <QEventLoop>
#include <QTimer>
#include <QTest>
namespace compat {
inline void put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("fixture write");
}
inline QByteArray bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("fixture read");
    return file.readAll();
}
inline configqml::ConfigResult request(configqml::ConfigStore &store,
                                       const std::function<quint64()> &dispatch) {
    QEventLoop loop;
    configqml::ConfigResult result;
    quint64 id = 0;
    const auto connection = QObject::connect(&store, &configqml::ConfigStore::completed, &loop,
                                             [&](const configqml::ConfigResult &receipt) {
                                                 if (receipt.requestId == id) {
                                                     result = receipt;
                                                     loop.quit();
                                                 }
                                             });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    id = dispatch();
    loop.exec();
    QObject::disconnect(connection);
    if (!result.requestId)
        qFatal("request timeout");
    return result;
}
} // namespace compat
