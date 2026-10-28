// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configfacade.h"
#include "configqmlvalue.h"
#include "configconsumer_p.h"
#include <QQmlEngine>
namespace configqml {
ConfigFacade::ConfigFacade(ConfigStore *store, ConfigPath prefix, QObject *parent)
    : QObject(parent), m_store(store), m_prefix(ownPath(prefix)) {
    connect(store, &ConfigStore::valuesChanged, this, [this](const auto &paths) {
        ConfigConsumerGuard boundary(this);
        for (const auto &path : paths)
            if (startsWith(path, m_prefix))
                if (!boundary.invoke([&] { emit settingChanged(path.mid(m_prefix.size())); }))
                    return;
    });
}
QVariant ConfigFacade::readSetting(const ConfigPath &relativePath) const {
    if (!m_store)
        return {};
    const auto path = m_prefix + relativePath;
    const auto d = m_store->schema().descriptor(path);
    return d ? qmlValue(*d, m_store->value(path)) : QVariant();
}
ConfigRequestHandle ConfigFacade::writeSetting(const ConfigPath &relativePath,
                                               const QVariant &value) {
    ConfigConsumerGuard boundary(this, m_store);
    const auto path = m_prefix + relativePath;
    const auto d = m_store ? m_store->schema().descriptor(path) : std::nullopt;
    ConfigValueResult checked;
    if (d) {
        if (!boundary.invoke([&] { checked = admitQmlValue(*d, value, path); }))
            return {};
    } else
        checked.errors.append(ConfigDiagnostic{path, "detached-or-undeclared"});
    ConfigRequestHandle handle;
    if (checked.ok()) {
        if (!boundary.invoke([&] { handle = m_store->setUser(path, checked.value); }))
            return handle;
    } else
        handle.errors = checked.errors;
    if (!handle.accepted) {
        if (!boundary.invoke([&] { emit inputRejected(relativePath, handle.errors.first().code); }))
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
