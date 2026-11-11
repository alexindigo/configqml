// SPDX-License-Identifier: LGPL-3.0-or-later
// The sole production translation unit using private Qt API. No private types
// escape it. Its exact Core/CorePrivate/Qml identity is checked by CMake.
#include "configqmlstore.h"
#include "configconsumer_p.h"
#include <QtCore/private/qmetaobjectbuilder_p.h>
#include <QtCore/private/qproperty_p.h>
#include <QQmlEngine>
#include <cstdlib>
#include <cstring>

namespace configqml {
// Compatibility bindables route binding-produced writes through the real setter.
// A plain QProperty here would let Qt.binding() bypass schema admission/intent.
class ConfigNamespaceValue final : public QObject {
public:
    ConfigNamespaceValue(ConfigQmlStore *store, ConfigPath path, QObject *parent)
        : QObject(parent), m_store(store), m_path(std::move(path)) {}
    QVariant read() const {
        (void)m_published.value();
        return m_store->read(m_path);
    }
    void write(const QVariant &value) {
        m_value.removeBindingUnlessInWrapper();
        m_store->write(m_path, value);
    }
    QUntypedBindable bindable();
    void prepare() {
        const auto value = m_store->read(m_path);
        m_value.setValueBypassingBindings(value);
        m_published.setValue(value);
    }

private:
    ConfigQmlStore *m_store;
    ConfigPath m_path;
    QProperty<QVariant> m_published;
    Q_OBJECT_COMPAT_PROPERTY_WITH_ARGS(ConfigNamespaceValue, QVariant, m_value,
                                       &ConfigNamespaceValue::write, nullptr,
                                       &ConfigNamespaceValue::read, QVariant())
};
QUntypedBindable ConfigNamespaceValue::bindable() {
    // Input evaluation belongs to the compatibility setter; effective-value
    // observers belong to publication. Attaching them to the input binding
    // would delay them until its setter returns, after synchronous store signals.
    using Input = decltype(m_value);
    static const auto interface = [] {
        auto result = QtPrivate::QBindableInterfaceForProperty<Input>::iface;
        result.setObserver = [](const QUntypedPropertyData *data, QPropertyObserver *observer) {
            auto *owner = reinterpret_cast<const ConfigNamespaceValue *>(
                reinterpret_cast<const char *>(data) - _qt_property_m_value_offset());
            observer->setSource(owner->m_published.bindingData());
        };
        return result;
    }();
    struct Bindable final : QUntypedBindable {
        Bindable(Input *input, const QtPrivate::QBindableInterface *interface)
            : QUntypedBindable(input, interface) {}
    };
    return Bindable(&m_value, &interface);
}
class ConfigNamespace final : public QObject {
public:
    ConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor, ConfigPath path,
                    QObject *parent)
        : QObject(parent), m_store(store), m_path(ownPath(path)) {
        QMetaObjectBuilder builder;
        builder.setClassName("ConfigQmlNamespace");
        builder.setSuperClass(&QObject::staticMetaObject);
        for (auto it = descriptor.fields.cbegin(); it != descriptor.fields.cend(); ++it) {
            const bool group = it->type == ConfigDescriptor::Type::Group;
            const auto childPath = m_path + ConfigPath{it.key()};
            m_names.append(ownString(it.key()));
            m_groups.append(group);
            m_children.append(group ? createConfigNamespace(store, it.value(), childPath, this)
                                    : nullptr);
            m_properties.append(group ? nullptr : new ConfigNamespaceValue(store, childPath, this));
            const auto signal = builder.addSignal(it.key().toUtf8() + "Changed()");
            auto property = builder.addProperty(it.key().toUtf8(), group ? "QObject*" : "QVariant",
                                                signal.index());
            property.setWritable(!group && store->canWrite(childPath));
            property.setConstant(group);
            property.setBindable(!group);
        }
        m_meta = builder.toMetaObject();
        QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    }
    void prepare(const QList<ConfigPath> &paths) {
        ConfigConsumerGuard boundary(this);
        for (const auto &child : m_children)
            if (child && !boundary.invoke([&] { prepareConfigNamespace(child, paths); }))
                return;
        for (const auto &path : paths)
            if (path.size() == m_path.size() + 1 && startsWith(path, m_path)) {
                const auto index = m_names.indexOf(path.last());
                if (index >= 0 && m_properties[index] &&
                    !boundary.invoke([&] { m_properties[index]->prepare(); }))
                    return;
            }
    }
    void notify(const QList<ConfigPath> &paths) {
        ConfigConsumerGuard boundary(this);
        for (const auto &child : m_children)
            if (child && !boundary.invoke([&] { notifyConfigNamespace(child, paths); }))
                return;
        for (const auto &path : paths)
            if (path.size() == m_path.size() + 1 && startsWith(path, m_path)) {
                const auto index = m_names.indexOf(path.last());
                if (index >= 0 && !m_groups[index] &&
                    !boundary.invoke([&] { QMetaObject::activate(this, m_meta, index, nullptr); }))
                    return;
            }
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
                    if (!boundary.invoke(
                            [&] { m_properties[index]->write(*static_cast<QVariant *>(args[0])); }))
                        return -1;
                }
            }
            index -= count;
        } else if (call == QMetaObject::BindableProperty) {
            if (index < count && !m_groups[index])
                *static_cast<QUntypedBindable *>(args[0]) = m_properties[index]->bindable();
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
    QList<ConfigNamespaceValue *> m_properties;
    QList<bool> m_groups;
    QMetaObject *m_meta = nullptr;
};
QObject *createConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor,
                               const ConfigPath &path, QObject *parent) {
    return new ConfigNamespace(store, descriptor, path, parent);
}
void notifyConfigNamespace(QObject *object, const QList<ConfigPath> &paths) {
    if (object)
        static_cast<ConfigNamespace *>(object)->notify(paths);
}
void prepareConfigNamespace(QObject *object, const QList<ConfigPath> &paths) {
    if (object)
        static_cast<ConfigNamespace *>(object)->prepare(paths);
}
} // namespace configqml
