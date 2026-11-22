// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlpendingreply.h"
#include "configqmlevent_p.h"
#include "configqmlhandle.h"
#include <QQmlEngine>

namespace configqml {
ConfigQmlPendingReply::ConfigQmlPendingReply(ConfigPendingReply *reply, QJSEngine *engine)
    : m_reply(reply), m_engine(engine) {
    Q_ASSERT(reply && !reply->parent() && reply->thread() == thread());
    reply->setParent(this);
    QQmlEngine::setObjectOwnership(this, QQmlEngine::JavaScriptOwnership);
    connect(reply, &ConfigPendingReply::finished, this, &ConfigQmlPendingReply::finished);
}
bool ConfigQmlPendingReply::isFinished() const { return m_reply->isFinished(); }
bool ConfigQmlPendingReply::isError() const { return m_reply->isError(); }
QJSValue ConfigQmlPendingReply::error() const {
    const auto error = m_reply->error();
    return qmlEvent(m_engine, {"code", "message", "phase"},
                    [error](const QString &key) -> QVariant {
                        if (key == "code")
                            return error.code();
                        if (key == "phase")
                            return error.phase();
                        return error.message();
                    });
}
QJSValue ConfigQmlPendingReply::value() const {
    const auto value = m_reply->value();
    const auto engine = m_engine;
    return qmlEvent(
        engine, {"handle", "generation", "snapshot", "observations", "pending", "write"},
        [engine, value](const QString &key) -> QVariant {
            if (key == "handle")
                return value.handle() ? QVariant::fromValue<QObject *>(
                                            ConfigQmlHandle::adopt(value.handle(), engine))
                                      : QVariant();
            if (key == "generation")
                return QVariant::fromValue(value.generation());
            if (key == "snapshot")
                return QVariant::fromValue(qmlReadSnapshotEvent(engine, value.snapshot()));
            if (key == "observations") {
                QVariantList results;
                for (const auto &read : value.observations()) {
                    const auto entry =
                        qmlEvent(engine, {"generation", "snapshot", "pending", "status", "error"},
                                 [engine, read](const QString &field) -> QVariant {
                                     if (field == "generation")
                                         return QVariant::fromValue(read.observation());
                                     return QVariant::fromValue(
                                         qmlReadResultEvent(engine, read).property(field));
                                 });
                    results.append(QVariant::fromValue(entry));
                }
                return results;
            }
            if (key == "pending") {
                QVariantList paths;
                for (const auto &path : value.pending())
                    paths.append(QVariant(path));
                return paths;
            }
            if (key == "write" && value.write())
                return QVariant::fromValue(
                    qmlResultEvent(engine, *value.write(), {}, value.generation()));
            return {};
        });
}
} // namespace configqml
