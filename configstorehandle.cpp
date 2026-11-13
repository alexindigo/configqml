// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configstorehandle.h"
#include "configconsumer_p.h"
#include "configshutdown_p.h"
namespace configqml {
ConfigStoreHandle::ConfigStoreHandle(ConfigStore *store, ConfigPath prefix, bool writable,
                                     QObject *parent)
    : QObject(parent), m_store(store), m_prefix(ownPath(prefix)), m_writable(writable) {
    if (store) {
        m_endpoint = store->m_owner->overlayEndpoint();
        m_diagnostics = store->schema().diagnostics();
    }
    const auto descriptor = store ? store->schema().descriptor(m_prefix) : std::nullopt;
    if (!descriptor || descriptor->type != ConfigDescriptor::Type::Group) {
        m_store = nullptr;
        return;
    }
    connect(store, &ConfigStore::valuesChanged, this, [this](const auto &paths) {
        QList<ConfigPath> relative;
        for (const auto &path : paths)
            if (startsWith(path, m_prefix))
                relative.append(path.mid(m_prefix.size()));
        if (!relative.isEmpty())
            ConfigConsumerGuard(this).invoke([&] { emit valuesChanged(relative); });
    });
}
ConfigStoreHandle::ConfigStoreHandle(ConfigDraft *draft, ConfigPath prefix, QObject *parent)
    : ConfigStoreHandle(draft->m_store, prefix, false, parent) {
    m_previewDraft = draft;
    m_previewId = draft->m_previewId;
    m_previewLifetime = draft->m_previewLifetime;
    disconnect(m_store, &ConfigStore::valuesChanged, this, nullptr);
    connect(draft, &ConfigDraft::previewValuesChanged, this, [this](const auto &paths) {
        if (!attached())
            return;
        QList<ConfigPath> relative;
        for (const auto &path : paths)
            if (startsWith(path, m_prefix))
                relative.append(path.mid(m_prefix.size()));
        if (!relative.isEmpty())
            ConfigConsumerGuard(this).invoke([&] { emit valuesChanged(relative); });
    });
    connect(draft, &ConfigDraft::previewScopesClosed, this, &ConfigStoreHandle::detached);
    connect(draft, &QObject::destroyed, this, &ConfigStoreHandle::detached);
}
ConfigStoreHandle::~ConfigStoreHandle() { invalidateLifetime(); }
void ConfigStoreHandle::invalidateLifetime() {
    if (!m_lifetime->exchange(false))
        return;
    if (auto endpoint = m_endpoint.lock())
        endpoint->send(
            [lifetime = m_lifetime](ConfigWorker &w) { w.overlayScopeClosed(lifetime); });
}
void ConfigStoreHandle::detach() {
    invalidateLifetime();
    if (m_store)
        disconnect(m_store, nullptr, this, nullptr);
    m_store = nullptr;
    ConfigConsumerGuard(this).invoke([&] { emit detached(); });
}
ConfigStoreHandle *ConfigStoreHandle::scope(const ConfigPath &relative, bool writable) {
    if (m_previewLifetime) {
        if (!attached())
            return nullptr;
        const auto descriptor = m_previewDraft->previewSchema().descriptor(m_prefix + relative);
        return descriptor && descriptor->type == ConfigDescriptor::Type::Group
                   ? new ConfigStoreHandle(m_previewDraft, m_prefix + relative, this)
                   : nullptr;
    }
    auto *child = new ConfigStoreHandle(m_store, m_prefix + relative, writable && m_writable, this);
    child->m_diagnostics = m_diagnostics;
    return child;
}
QVariant ConfigStoreHandle::value(const ConfigPath &relative) const {
    if (m_previewLifetime)
        return attached() ? m_previewDraft->previewValue(m_prefix + relative) : QVariant();
    return m_store ? m_store->value(m_prefix + relative) : QVariant();
}
ConfigPathState ConfigStoreHandle::info(const ConfigPath &relative) const {
    if (m_previewLifetime &&
        (!attached() || !m_previewDraft->previewSchema().descriptor(m_prefix + relative)))
        return {};
    auto result = m_store ? m_store->info(m_prefix + relative) : ConfigPathState{};
    if (!m_writable)
        result.provenance.route = {};
    return result;
}
ConfigListModel *ConfigStoreHandle::listModel(const ConfigPath &relative) const {
    if (m_previewLifetime)
        return nullptr;
    return m_store ? m_store->listModel(m_prefix + relative) : nullptr;
}
ConfigRequestHandle ConfigStoreHandle::setUser(const ConfigPath &relative, const QVariant &value,
                                               ConfigStore::Callback callback) {
    if (!m_store || !m_writable)
        return {0, 0, false, {m_diagnostics.make(m_prefix + relative, "detached-or-read-only")}};
    QPointer<ConfigStoreHandle> guard(this);
    return m_store->setUser(m_prefix + relative, value, this,
                            [guard, callback = std::move(callback)](const ConfigResult &result) {
                                if (guard && guard->attached() && callback)
                                    callback(result);
                            });
}
quint64 ConfigStoreHandle::save(ConfigStore::Callback callback) {
    if (!m_store || !m_writable)
        return 0;
    QPointer<ConfigStoreHandle> guard(this);
    return m_store->save(this, [guard, callback = std::move(callback)](const ConfigResult &result) {
        if (guard && guard->attached() && callback)
            callback(result);
    });
}
ConfigDraft *ConfigStoreHandle::draft(const QList<ConfigPath> &paths) {
    if (!m_store || !m_writable)
        return nullptr;
    QList<ConfigPath> selection;
    if (paths.isEmpty())
        selection.append(m_prefix);
    for (const auto &path : paths)
        selection.append(m_prefix + path);
    return m_store->draft(selection, this);
}
ConfigStoreHandle *ConfigStore::scope(const ConfigPath &prefix, bool writable, QObject *parent) {
    return new ConfigStoreHandle(this, prefix, writable, parent ? parent : this);
}
} // namespace configqml
