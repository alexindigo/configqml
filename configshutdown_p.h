// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
// Process-owned worker lifetime, independent of every consumer QObject.
#include "configworker_p.h"
#include <QMutex>

namespace configqml {
class ConfigWorkerOwner : public QObject, public std::enable_shared_from_this<ConfigWorkerOwner> {
    Q_OBJECT
public:
    ConfigWorkerOwner(ConfigStoreOptions options, const QString &initializationError);
    void start();
    quint64 reserveRequest();
    bool dispatch(const ConfigCommand &command);
    bool accepting() const;
    void detach();
    void settleForExit();
    bool stopped() const;
    void pollJoin();
    void reclaim(std::shared_ptr<const ConfigSnapshot> snapshot);
    void acknowledgeModels(const QMap<ConfigPath, quint64> &generations);
    QString description() const;
    ConfigWorker *workerForTest() const { return m_worker; }
signals:
    void packetReady(const configqml::ConfigWorkerPacket &packet);
    void implicitClose(const configqml::ConfigCommand &command);
    void joined();

private:
    void settle(bool detached);
    void receive(const ConfigWorkerPacket &packet);
    mutable QMutex m_mutex;
    ConfigWorker *m_worker;
    QThread *m_thread;
    QTimer *m_joinTimer;
    QString m_storeId;
    bool m_autoSave;
    bool m_sealed = false;
    bool m_terminalQueued = false;
    bool m_terminalReceived = false;
    bool m_stopped = false;
    bool m_detached = false;
    quint64 m_prefix = 0;
    quint64 m_requestId = 0;
    int m_pendingPersistence = 0;
    std::optional<ConfigResult> m_outcome;
};
} // namespace configqml
