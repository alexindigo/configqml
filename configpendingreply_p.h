// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configpendingreply.h"
#include "configreadprojection.h"
#include <QPointer>

namespace configqml {
struct ConfigReplyErrorData {
    QString code, message, phase;
};
struct ConfigReplyValueData {
    ConfigObservation generation;
    ConfigReadSnapshot snapshot;
    QList<ConfigReadResult> observations;
    QList<ConfigPath> pending;
    std::optional<ConfigResult> write;
};
// One native envelope for future handle verbs. It never captures values itself:
// all reads/install/notification fencing remain in ConfigReadProjection's P3 owner.
class ConfigPendingReplyState {
public:
    static ConfigPendingReply *read(ConfigReadProjection *initiator,
                                    const ConfigObservation &after = {},
                                    const QList<ConfigReadProjection *> &observe = {},
                                    QObject *parent = nullptr);
    static ConfigPendingReply *refuse(const QString &code, const QString &phase,
                                      QObject *parent = nullptr);

private:
    friend class ConfigPendingReply;
    explicit ConfigPendingReplyState(ConfigPendingReply *reply);
    void start(const ConfigObservation &after);
    void acquired(qsizetype index, const ConfigReadResult &result);
    void settle(const QString &code = {}, const QString &phase = {});
    void finish();
    ConfigPendingReply *q;
    bool finished = false, queued = false;
    ConfigReplyError error;
    ConfigReplyValue value;
    ConfigReplyValueData captured;
    QList<QPointer<ConfigReadProjection>> views;
    QList<QMetaObject::Connection> lifetimes;
    qsizetype outstanding = 0;
};
} // namespace configqml
