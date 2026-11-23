// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configpendingreply.h"
#include "configreadprojection.h"
#include "confighandle.h"
#include "configobservation_p.h"
#include <QPointer>

namespace configqml {
struct ConfigReplyErrorData {
    QString code, message, phase;
};
struct ConfigReplyValueData {
    QPointer<ConfigHandle> handle;
    ConfigObservation generation;
    ConfigReadSnapshot snapshot;
    QList<ConfigReadResult> observations;
    QList<ConfigPath> pending;
    std::optional<ConfigResult> write;
};
// The writer half of a composed operation: the owner's own admission/staging
// outcome and its captured coverage, delivered through the paired envelope.
struct ConfigReplyWrite {
    std::optional<ConfigResult> result;
    ConfigObservation observation;
    QString code, phase;      // Local refusal before any effect was admitted.
    bool persistence = false; // Apply/save: a failed result is a publication failure.
};
using ConfigReplyWriter =
    std::function<void(QObject *context, std::function<void(const ConfigReplyWrite &)> done)>;
// One native envelope for every handle verb. It never captures values itself:
// all reads/install/notification fencing remain in ConfigReadProjection's P3 owner.
class ConfigPendingReplyState {
public:
    static ConfigPendingReply *read(ConfigReadProjection *initiator,
                                    const ConfigObservation &after = {},
                                    const QList<ConfigReadProjection *> &observe = {},
                                    QObject *parent = nullptr);
    // Views are captured and validated before the writer admits anything. A
    // barrier initiator is synchronized after the write; a staged (frozen)
    // initiator contributes its installed candidate instead.
    static ConfigPendingReply *
    operation(ConfigReadProjection *initiator, bool barrier, const ConfigObservation &after,
              const QList<ConfigReadProjection *> &observe, ConfigReplyWriter writer,
              ConfigHandle *opened = nullptr, const ConfigObservation &shape = {},
              QObject *parent = nullptr, QObject *retainedWriterOwner = nullptr);
    static ConfigPendingReply *refuse(const QString &code, const QString &phase,
                                      QObject *parent = nullptr);

private:
    friend class ConfigPendingReply;
    explicit ConfigPendingReplyState(ConfigPendingReply *reply);
    void start(const ConfigObservation &after);
    void written(const ConfigReplyWrite &write, const ConfigObservation &after);
    void staged();
    void observeViews();
    void acquired(qsizetype index, const ConfigReadResult &result);
    void viewLost(qsizetype index);
    void settle(const QString &code = {}, const QString &phase = {});
    void finish();
    ConfigPendingReply *q;
    bool finished = false, queued = false, barrier = true;
    bool retainedWriter = false, awaitingWriter = false, readerLost = false;
    ConfigReplyError error;
    ConfigReplyValue value;
    ConfigReplyValueData captured;
    QList<QPointer<ConfigReadProjection>> views;
    QList<QMetaObject::Connection> lifetimes;
    qsizetype outstanding = 0;
};
} // namespace configqml
