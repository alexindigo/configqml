// SPDX-License-Identifier: LGPL-3.0-or-later
// Compatibility QML adapter; schema handles use the same native read capability.
#pragma once
#include "configreadprojection.h"
#include <QJSValue>
#include <QJSEngine>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace configqml {
class ConfigQmlReadProjection : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
public:
    ConfigQmlReadProjection(ConfigReadProjection *projection, QJSEngine *engine, QObject *parent);
    void bind(ConfigReadProjection *projection, QJSEngine *engine);
    Q_INVOKABLE QJSValue state(const QStringList &path) const;
    Q_INVOKABLE QString synchronize(const QJSValue &callback = {});
    Q_INVOKABLE QString synchronize(const QVariant &after, const QJSValue &callback);
signals:
    void stateChanged();
    void completed(const QJSValue &result);

private:
    QPointer<ConfigReadProjection> m_projection;
    QPointer<QJSEngine> m_engine;
};
} // namespace configqml
