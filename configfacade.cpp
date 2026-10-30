// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configfacade.h"
#include "configqmlvalue.h"
#include "configconsumer_p.h"
#include <QQmlEngine>
namespace configqml {
ConfigFacade::ConfigFacade(ConfigStore *store, ConfigPath prefix, QObject *parent)
    : QObject(parent), m_store(store), m_prefix(ownPath(prefix)) {
    if (store) {
        m_diagnostics = store->schema().diagnostics();
        connect(store, &ConfigStore::valuesChanged, this, [this](const auto &paths) {
            ConfigConsumerGuard boundary(this);
            for (const auto &path : paths)
                if (startsWith(path, m_prefix))
                    if (!boundary.invoke([&] { emit settingChanged(path.mid(m_prefix.size())); }))
                        return;
        });
    }
}
QVariant ConfigFacade::readSetting(const ConfigPath &relativePath) const {
    if (!m_store)
        return {};
    const auto path = m_prefix + relativePath;
    return m_store->presentation(path);
}
ConfigRequestHandle ConfigFacade::writeSetting(const ConfigPath &relativePath,
                                               const QVariant &value) {
    ConfigConsumerGuard boundary(this, m_store);
    const auto path = m_prefix + relativePath;
    ConfigRequestHandle handle;
    if (m_store) {
        const auto schema = m_store->schema();
        ConfigAdmission checked;
        if (!boundary.invoke([&] { checked = admitQmlInput(schema, path, value); }))
            return {};
        if (!boundary.invoke([&] { handle = m_store->setUser(checked); }))
            return handle;
    } else
        handle.errors = {m_diagnostics.make(path, "detached-or-undeclared")};
    if (!handle.accepted) {
        const auto safePath = m_diagnostics.path(path).mid(m_prefix.size());
        if (!boundary.invoke([&] { emit inputRejected(safePath, handle.errors.first().code); }))
            return handle;
        boundary.invoke([&] {
            if (auto *engine = qjsEngine(this))
                engine->throwError(QJSValue::TypeError,
                                   QString("configqml: ") + handle.errors.first().code);
        });
    }
    return handle;
}
} // namespace configqml
