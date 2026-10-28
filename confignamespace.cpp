// SPDX-License-Identifier: LGPL-3.0-or-later
// The sole production translation unit using private Qt API. No private types
// escape it. Its exact Core/CorePrivate/Qml identity is checked by CMake.
#include "configqmlstore.h"
#include "configconsumer_p.h"
#include <QtCore/private/qmetaobjectbuilder_p.h>
#include <QQmlEngine>
#include <cstdlib>
#include <cstring>

namespace configqml {
class ConfigNamespace final : public QObject {
public:
    ConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor, ConfigPath path,
                    QObject *parent)
        : QObject(parent), m_store(store), m_path(std::move(path)) {
        QMetaObjectBuilder builder;
        builder.setClassName("ConfigQmlNamespace");
        builder.setSuperClass(&QObject::staticMetaObject);
        for (auto it = descriptor.fields.cbegin(); it != descriptor.fields.cend(); ++it) {
            const bool group = it->type == ConfigDescriptor::Type::Group;
            const auto childPath = m_path + ConfigPath{it.key()};
            m_names.append(it.key());
            m_groups.append(group);
            m_children.append(group ? createConfigNamespace(store, it.value(), childPath, this)
                                    : nullptr);
            const auto signal = builder.addSignal(it.key().toUtf8() + "Changed()");
            auto property = builder.addProperty(it.key().toUtf8(), group ? "QObject*" : "QVariant",
                                                signal.index());
            property.setWritable(!group && store->core()->canWriteUser(childPath));
            property.setConstant(group);
        }
        m_meta = builder.toMetaObject();
        QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
        connect(store->core(), &ConfigStore::valuesChanged, this, [this](const auto &paths) {
            ConfigConsumerGuard boundary(this);
            for (const auto &path : paths)
                if (path.size() == m_path.size() + 1 && startsWith(path, m_path)) {
                    const auto index = m_names.indexOf(path.last());
                    if (index >= 0 && !m_groups[index])
                        if (!boundary.invoke(
                                [&] { QMetaObject::activate(this, m_meta, index, nullptr); }))
                            return;
                }
        });
    }
    ~ConfigNamespace() override { std::free(m_meta); }
    const QMetaObject *metaObject() const override {
        return m_meta ? m_meta : &QObject::staticMetaObject;
    }
    void *qt_metacast(const char *name) override {
        return !std::strcmp(name, "ConfigQmlNamespace") ? this : QObject::qt_metacast(name);
    }
    int qt_metacall(QMetaObject::Call call, int index, void **args) override {
        index = QObject::qt_metacall(call, index, args);
        if (index < 0)
            return index;
        const auto count = m_names.size();
        if (call == QMetaObject::ReadProperty || call == QMetaObject::WriteProperty) {
            if (index < count) {
                if (call == QMetaObject::ReadProperty) {
                    if (m_groups[index])
                        *static_cast<QObject **>(args[0]) = m_children[index];
                    else
                        *static_cast<QVariant *>(args[0]) =
                            m_store->read(m_path + ConfigPath{m_names[index]});
                } else if (!m_groups[index]) {
                    ConfigConsumerGuard boundary(this);
                    if (!boundary.invoke([&] {
                            m_store->write(m_path + ConfigPath{m_names[index]},
                                           *static_cast<QVariant *>(args[0]));
                        }))
                        return -1;
                }
            }
            index -= count;
        } else if (call == QMetaObject::InvokeMetaMethod)
            index -= count;
        return index;
    }

private:
    ConfigQmlStore *m_store;
    ConfigPath m_path;
    QStringList m_names;
    QList<QPointer<QObject>> m_children;
    QList<bool> m_groups;
    QMetaObject *m_meta = nullptr;
};
QObject *createConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor,
                               const ConfigPath &path, QObject *parent) {
    return new ConfigNamespace(store, descriptor, path, parent);
}
} // namespace configqml
