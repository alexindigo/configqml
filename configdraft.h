// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configstore.h"

namespace configqml {
// Detached editor. Its values are not an optimistic store layer.
class ConfigDraft : public QObject {
    Q_OBJECT
public:
    ConfigDraft(ConfigStore *store, const QList<ConfigPath> &paths, QObject *parent = nullptr);
    bool valid() const { return m_error.isEmpty() && m_store; }
    QString error() const { return m_error; }
    bool dirty() const { return !m_edits.isEmpty(); }
    bool conflicted() const { return m_conflicted; }
    bool busy() const { return m_busy; }
    ConfigSchema schema() const { return m_schema; }
    QList<ConfigPath> paths() const { return m_base.keys(); }
    QVariant value(const ConfigPath &path) const;
    QVariant presentation(const ConfigPath &path) const;
    ConfigOperation setValue(const ConfigPath &path, const QVariant &value,
                             ConfigDescriptor::Format format = ConfigDescriptor::Format::Native);
    ConfigOperation setValue(const ConfigAdmission &value);
    quint64 apply(QObject *context = nullptr, ConfigStore::Callback callback = {});
    bool discard();
signals:
    void valuesChanged(const QList<configqml::ConfigPath> &paths);
    void stateChanged();

private:
    void refresh();
    void observe();
    QPointer<ConfigStore> m_store;
    ConfigSchema m_schema;
    QString m_error;
    QMap<ConfigPath, QVariant> m_base;
    QMap<ConfigPath, ConfigPathState> m_fences;
    QMap<ConfigPath, std::optional<QVariant>> m_edits;
    mutable QMap<ConfigPath, QVariant> m_presentations;
    bool m_conflicted = false;
    bool m_busy = false;
};
} // namespace configqml
