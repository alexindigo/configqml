// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configworker_p.h"

namespace configqml {
class ConfigStoreTestAccess {
public:
    static qint64 planningNanos(const ConfigStore &store) { return store.m_lastPlanningNanos; }
    static qint64 publicationLibraryNanos(const ConfigStore &store) {
        return store.m_lastLibraryPublicationNanos;
    }
    static qint64 modelLibraryNanos(const ConfigListModel &model) {
        return model.m_lastLibraryNanos;
    }
    static qint64 modelNotificationNanos(const ConfigListModel &model) {
        return model.m_lastNotificationNanos;
    }
    static void plainReset(ConfigStore &store, ConfigListModel &model) {
        // Timing control: exactly the public Qt reset primitive, identical current payload.
        ConfigModelPlan plan;
        plan.base = plan.generation = model.m_generation;
        plan.reset = true;
        for (auto it = store.m_models.cbegin(); it != store.m_models.cend(); ++it)
            if (it.value().get() == &model) {
                plan.rows = projectionRows(model.descriptor(), store.value(it.key()));
                for (int i = 0; i < plan.rows.size(); ++i)
                    plan.rowIndices.insert(plan.rows[i].id.toString(), i);
            }
        model.apply(plan, &store);
    }
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
