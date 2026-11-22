// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configpendingreply.h"
#include <QJSEngine>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace configqml {
class ConfigQmlPendingReply : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool isFinished READ isFinished NOTIFY finished)
    Q_PROPERTY(bool isError READ isError NOTIFY finished)
    Q_PROPERTY(QJSValue error READ error NOTIFY finished)
    Q_PROPERTY(QJSValue value READ value NOTIFY finished)
public:
    // Takes ownership of an unparented native reply. The wrapper has no store
    // parent and is reclaimable by the engine once the caller releases it.
    ConfigQmlPendingReply(ConfigPendingReply *reply, QJSEngine *engine);
    bool isFinished() const;
    bool isError() const;
    QJSValue error() const;
    QJSValue value() const;
signals:
    void finished();

private:
    ConfigPendingReply *m_reply;
    QPointer<QJSEngine> m_engine;
};
} // namespace configqml
