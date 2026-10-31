// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configdraft.h"
#include <atomic>

namespace configqml {
struct ConfigOverlayEndpoint;
class ConfigStoreHandle : public QObject {
    Q_OBJECT
public:
    ConfigStoreHandle(ConfigStore *store, ConfigPath prefix, bool writable,
                      QObject *parent = nullptr);
    ~ConfigStoreHandle() override;
    bool attached() const { return m_store; }
    void detach();
    ConfigStoreHandle *scope(const ConfigPath &relative, bool writable = true);
    QVariant value(const ConfigPath &relative) const;
    ConfigPathState info(const ConfigPath &relative) const;
    ConfigListModel *listModel(const ConfigPath &relative) const;
    ConfigRequestHandle setUser(const ConfigPath &relative, const QVariant &value,
                                ConfigStore::Callback callback = {});
    quint64 save(ConfigStore::Callback callback = {});
    ConfigDraft *draft(const QList<ConfigPath> &paths = {});
signals:
    void valuesChanged(const QList<configqml::ConfigPath> &paths);
    void detached();

private:
    friend class ConfigStore;
    void invalidateLifetime();
    std::weak_ptr<ConfigOverlayEndpoint> m_endpoint;
    std::shared_ptr<std::atomic_bool> m_lifetime = std::make_shared<std::atomic_bool>(true);
    QPointer<ConfigStore> m_store;
    ConfigPath m_prefix;
    bool m_writable;
    ConfigDiagnosticBuilder m_diagnostics;
};
} // namespace configqml
