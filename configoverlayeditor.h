// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configstore.h"

namespace configqml {
enum class ConfigOverlayMode { View, Editor, Live };
enum class ConfigOverlayMergePolicy { RecursiveObjectsReplaceLists };
enum class ConfigOverlayDifferencePolicy { TopLevelOmitEqual };
struct ConfigOverlayOptions {
    ConfigOverlayMode mode = ConfigOverlayMode::Editor;
    ConfigStoreHandle *base = nullptr; // Resolved on the caller's object thread only.
    ConfigPath ownerPath;
    quint64 ownerGeneration = 0;
    QList<ConfigValueSelector> entryPath, identityPath;
    QVariant identityValue;
    QString overrideKey;
    ConfigOverlayMergePolicy mergePolicy = ConfigOverlayMergePolicy::RecursiveObjectsReplaceLists;
    ConfigOverlayDifferencePolicy differencePolicy =
        ConfigOverlayDifferencePolicy::TopLevelOmitEqual;
};
struct ConfigOverlayPresentation;
class ConfigOverlayEditor : public QObject {
    Q_OBJECT
public:
    enum class Status { Loading, Ready, Error, Closed };
    Q_ENUM(Status)
    ~ConfigOverlayEditor() override;
    QString editorId() const { return m_id; }
    ConfigSchema schema() const { return m_schema; }
    QVariant value(const ConfigPath &path) const;
    QVariant presentation(const ConfigPath &path) const;
    ConfigRequestHandle setValue(const ConfigPath &path, const QVariant &value);
    ConfigRequestHandle setValue(const ConfigAdmission &admission);
    bool unset(const ConfigPath &path);
    bool ready() const { return m_status == Status::Ready; }
    bool dirty() const;
    bool conflicted() const { return m_conflicted; }
    bool busy() const { return m_busy; }
    bool writable() const { return m_mode != ConfigOverlayMode::View; }
    Status status() const { return m_status; }
    QString error() const { return m_error; }
    quint64 apply(QObject *context = nullptr, ConfigStore::Callback callback = {});
    quint64 resetOverrides(QObject *context = nullptr, ConfigStore::Callback callback = {});
    quint64 discard(QObject *context = nullptr, ConfigStore::Callback callback = {});
    quint64 save(QObject *context = nullptr, ConfigStore::Callback callback = {});
    quint64 flush(QObject *context = nullptr, ConfigStore::Callback callback = {});
    void detach();
signals:
    void valuesChanged(const QList<configqml::ConfigPath> &paths);
    void stateChanged();
    void completed(const configqml::ConfigResult &result);

private:
    friend class ConfigStore;
    ConfigOverlayEditor(ConfigStore *store, const ConfigOverlayPresentation &presentation);
    void install(const ConfigOverlayPresentation &presentation);
    quint64 request(const QString &operation, QObject *context, ConfigStore::Callback callback);
    ConfigRequestHandle writeLive(const ConfigPath &path, const std::optional<QVariant> &value);
    QPointer<ConfigStore> m_store;
    QString m_id, m_error;
    ConfigPath m_ownerPath;
    ConfigSchema m_schema{ConfigDescriptor::group({})};
    ConfigOverlayMode m_mode = ConfigOverlayMode::Editor;
    Status m_status = Status::Loading;
    QMap<ConfigPath, QVariant> m_values;
    QMap<ConfigPath, QVariant> m_baseValues;
    QMap<ConfigPath, quint64> m_editSequences;
    QMap<ConfigPath, std::optional<QVariant>> m_edits;
    mutable QMap<ConfigPath, QVariant> m_presentations;
    bool m_conflicted = false, m_busy = false, m_pendingReset = false;
    bool m_liveDirty = false;
};
} // namespace configqml
