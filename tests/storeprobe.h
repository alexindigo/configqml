// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configworker_p.h"
#include "configshutdown_p.h"
#include "configchange_p.h"

namespace configqml {
class ConfigStoreTestAccess {
public:
    static void observeOverlayPreparation(ConfigOverlayEditor &editor, QObject *context,
                                          ConfigChangePublisher::Callback callback) {
        editor.m_changes->add(context, std::move(callback));
    }
    static void post(ConfigWorker &worker, const ConfigWorkerPacket &packet) {
        worker.post(packet);
    }
    static ConfigWorkerPacket publication(ConfigWorker &worker) { return worker.publish(); }
    static qsizetype evidenceBacklogLimit() { return ConfigWorker::EvidenceBacklogLimit; }
    static bool ordinaryOwnPublication(const ConfigWorker &worker, const QString &source) {
        return worker.m_ownPublications.value(source).contains(QString());
    }
    static QString acquireEvidence(ConfigWorker &worker, const QString &source) {
        const auto capacity = worker.prepareEvidence(source, 1);
        if (!capacity.isEmpty())
            return capacity;
        auto *backend = worker.jsonBackend();
        const auto id = backend->submit({ConfigBackendRequest::Operation::Read, source, {}, {}});
        return worker.retainObservation(source, *backend->take(id), "read", "foreign");
    }
    static QString retainUnreservedEvidence(ConfigWorker &worker, const QString &source,
                                            const ConfigBackendResult &read) {
        return worker.retainObservation(source, read, "read", "foreign");
    }
    static qsizetype frozenIntents(const ConfigWorker &worker, const QString &source) {
        qsizetype count = 0;
        for (const auto &intent : worker.m_intents.value(source))
            count += bool(intent.frozen);
        return count;
    }
    static QList<ConfigEvidenceObservation> pendingEvidence(const ConfigWorker &worker,
                                                            const QString &source) {
        QList<ConfigEvidenceObservation> result;
        for (const auto &pending : worker.m_pendingEvidence.value(source))
            result.append(pending.observation);
        return result;
    }
    static quint64 endpointRevision(const ConfigStore &store, const ConfigPath &path) {
        return store.m_owner->overlayEndpoint()->revision(path);
    }
    static std::function<void()> backendRelease(ConfigStore &store) {
        QPointer<ConfigWorker> worker = store.m_worker;
        return [worker] {
            if (worker)
                QMetaObject::invokeMethod(
                    worker,
                    [worker] {
                        if (worker)
                            worker->releaseBackend();
                    },
                    Qt::QueuedConnection);
        };
    }
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
