// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configstore.h"
#include <atomic>

namespace configqml {
class ConfigStoreHandle;
struct ConfigApplyWitness;
// Detached editor. Its values are not an optimistic store layer.
class ConfigDraft : public QObject {
    Q_OBJECT
public:
    ConfigDraft(ConfigStore *store, const QList<ConfigPath> &paths, QObject *parent = nullptr);
    ~ConfigDraft() override;
    bool valid() const { return m_error.isEmpty() && m_store; }
    QString error() const { return m_error; }
    bool dirty() const { return !m_edits.isEmpty(); }
    bool conflicted() const { return m_conflicted; }
    bool busy() const { return m_busy; }
    ConfigSchema schema() const { return m_schema; }
    QList<ConfigPath> paths() const { return m_base.keys(); }
    QVariant value(const ConfigPath &path) const;
    QVariant presentation(const ConfigPath &path) const;
    QVariant previewValue(const ConfigPath &path) const;
    QVariant previewPresentation(const ConfigPath &path) const;
    ConfigSchema previewSchema() const { return m_previewSchema; }
    ConfigStoreHandle *previewScope(const ConfigPath &path, QObject *parent = nullptr);
    ConfigOperation setValue(const ConfigPath &path, const QVariant &value,
                             ConfigDescriptor::Format format = ConfigDescriptor::Format::Native);
    ConfigOperation setValue(const ConfigAdmission &value);
    quint64 apply(QObject *context = nullptr, ConfigStore::Callback callback = {});
    bool discard();
signals:
    void valuesChanged(const QList<configqml::ConfigPath> &paths);
    void stateChanged();
    void previewValuesChanged(const QList<configqml::ConfigPath> &paths);
    void previewScopesClosed();

private:
    friend class ConfigQmlStore;
    friend class ConfigStore;
    std::unique_ptr<ConfigChangePublisher> m_changes, m_previewChanges;
    bool prepareChanges(const QList<ConfigPath> &paths);
    friend class ConfigStoreHandle;
    void publishPreview();
    void closePreviewScopes();
    void refresh();
    void prepareStoreChange();
    void observe();
    QPointer<ConfigStore> m_store;
    ConfigSchema m_schema;
    ConfigSchema m_previewSchema{ConfigDescriptor::group({})};
    QString m_previewId;
    std::shared_ptr<std::atomic_bool> m_previewLifetime;
    QString m_error;
    QMap<ConfigPath, QVariant> m_base;
    QMap<ConfigPath, ConfigPathState> m_fences;
    QMap<ConfigPath, std::optional<QVariant>> m_edits;
    QString m_applyId;
    std::shared_ptr<ConfigApplyWitness> m_admitted;
    QMap<ConfigPath, std::optional<QVariant>> m_admittedEdits;
    mutable QMap<ConfigPath, QVariant> m_presentations;
    mutable QMap<ConfigPath, QVariant> m_previewPresentations;
    bool m_conflicted = false;
    bool m_busy = false;
    bool m_stateNotificationPending = false;
};
} // namespace configqml
