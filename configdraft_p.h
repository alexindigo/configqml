// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include "configresult.h"
#include "configchange_p.h"
#include "configobservation.h"
#include <atomic>
#include <memory>

namespace configqml {
// Immutable checked draft edits, never a copied store/raw tree or persistence layer.
struct ConfigDraftPreview {
    QString id;
    quint64 epoch = 0;
    std::shared_ptr<std::atomic_bool> lifetime;
    QList<ConfigPath> paths;
    QMap<ConfigPath, std::optional<QVariant>> edits;
};
struct ConfigDraftRequest {
    QString id;
    std::shared_ptr<std::atomic_bool> lifetime;
    quint64 epoch = 0;
    QList<ConfigPath> paths;
    QMap<ConfigPath, ConfigPathState> fences;
    QMap<ConfigPath, quint64> ingress;
    QMap<ConfigPath, std::optional<QVariant>> edits;
    QString previewId;
    std::shared_ptr<std::atomic_bool> previewLifetime;
    quint64 previewEpoch = 0;
};
struct ConfigDraftPresentation {
    std::shared_ptr<const ConfigDraftRequest> request;
    quint64 requestId = 0, generation = 0;
    QString error;
    QMap<ConfigPath, QVariant> frozen, live;
    ConfigReadSnapshot frozenRead, liveRead;
};
class ConfigDraft;
class ConfigStore;
struct ConfigWorkerPacket;
// QObject child storage preserves the exported ConfigDraft/ConfigStore layouts.
class ConfigDraftState : public QObject {
public:
    explicit ConfigDraftState(ConfigDraft *draft);
    ~ConfigDraftState() override;
    static ConfigDraftState *get(const ConfigDraft *draft);
    static void install(ConfigStore *store, const ConfigWorkerPacket &packet,
                        QList<ConfigChangePublisher::Change> &preparations);
    bool hasRemovals() const;
    bool settled() const { return m_settled; }
    bool resolving() const { return m_acceptedEpoch != m_epoch; }
    QString error() const { return m_error; }
    bool takeValuesChanged() { return std::exchange(m_valuesPending, false); }
    QVariant frozen(const ConfigPath &path) const { return m_frozen.value(path); }
    QVariant live(const ConfigPath &path) const { return m_live.value(path); }
    ConfigReadSnapshot read(bool preview) const { return preview ? m_liveRead : m_frozenRead; }
    bool current(const ConfigDraftRequest &request) const {
        return request.lifetime == m_lifetime && *m_lifetime && request.epoch == m_epoch;
    }
    void capture(const QMap<ConfigPath, std::optional<QVariant>> &edits);
    quint64 resolve(const QMap<ConfigPath, std::optional<QVariant>> &edits, bool stage,
                    QObject *context = nullptr,
                    std::function<void(const ConfigResult &)> callback = {},
                    const QString &failure = {});
    void reset();
    quint64 nextPreviewEpoch() { return ++m_previewEpoch; }
    quint64 previewEpoch() const { return m_previewEpoch; }
    QString id() const { return m_id; }

private:
    quint64 resolveRequest(const QMap<ConfigPath, std::optional<QVariant>> &edits, bool stage,
                           QObject *context, std::function<void(const ConfigResult &)> callback,
                           const QString &failure, bool control);
    ConfigDraft *m_draft;
    QString m_id;
    QString m_error;
    std::shared_ptr<std::atomic_bool> m_lifetime;
    quint64 m_epoch = 0, m_acceptedEpoch = 0, m_generation = 0, m_pending = 0;
    quint64 m_previewEpoch = 0;
    bool m_settled = true, m_removals = false;
    bool m_pendingStage = false, m_previousSettled = true, m_valuesPending = false;
    QMap<ConfigPath, QVariant> m_frozen, m_live;
    ConfigReadSnapshot m_frozenRead, m_liveRead;
};
ConfigSchema draftPreviewSchema(const ConfigSchema &schema, const QList<ConfigPath> &paths);
} // namespace configqml
