// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configbackend.h"
#include <QMap>
#include <QThread>
#include <QList>

namespace configqml {
class ConfigMemoryBackend final : public ConfigBackend {
public:
    enum class Fault { None, BeforeOperation, AfterPublication, Readback };
    struct Schedule {
        bool delayed = false;
        Fault fault = Fault::None;
    };
    ConfigMemoryBackend();
    ConfigMemoryBackend(const ConfigMemoryBackend &) = delete;
    ConfigMemoryBackend &operator=(const ConfigMemoryBackend &) = delete;
    ConfigBackendCapabilities capabilities() const override;
    quint64 submit(const ConfigBackendRequest &request) override;
    std::optional<ConfigBackendResult> take(quint64 requestId) override;
    // Deterministic controls, no timers/sleeps/global state. Owner thread only.
    bool schedule(const Schedule &schedule);
    bool release(quint64 requestId);
    bool seed(const QString &sourceId, const QByteArray &bytes, bool exists = true);
    QList<quint64> pending() const;

private:
    struct Entry {
        QByteArray bytes;
        ConfigRevision revision;
    };
    struct Pending {
        ConfigBackendRequest request;
        Schedule schedule;
    };
    bool onOwner() const;
    ConfigBackendResult execute(quint64 id, const Pending &pending);
    Qt::HANDLE m_owner;
    quint64 m_nextRequest = 0;
    quint64 m_nextRevision = 0;
    QMap<QString, Entry> m_entries;
    QList<Schedule> m_schedules;
    QMap<quint64, Pending> m_pending;
    QMap<quint64, ConfigBackendResult> m_completed;
};
} // namespace configqml
