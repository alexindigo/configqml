// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configworker_p.h"

namespace configqml {
class ConfigStoreTestAccess {
public:
    static qint64 worstPublicationNanos(const ConfigStore &store) {
        return store.m_worstPublicationNanos;
    }
    static void inject(ConfigStore &store, const ConfigWorkerPacket &packet) {
        store.receive(packet);
    }
    static void run(ConfigStore &store, std::function<void(ConfigWorker &)> action,
                    QObject *context = nullptr, std::function<void()> done = {}) {
        auto *worker = store.m_worker;
        QPointer<QObject> guard(context);
        QMetaObject::invokeMethod(
            worker,
            [worker, action = std::move(action), guard, done = std::move(done)] {
                action(*worker);
                if (guard && done)
                    QMetaObject::invokeMethod(guard, done, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
};
} // namespace configqml
