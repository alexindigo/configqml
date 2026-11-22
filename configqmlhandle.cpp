// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlhandle.h"
#include "confighandle_p.h"
#include "configqmlevent_p.h"
#include "configqmlstore.h"
#include "configqmloverlay.h"
#include "configstorehandle.h"
#include <QQmlEngine>

namespace configqml {
namespace {
// One immutable, prototype-free lookup object per handle and kind. Keys are
// opaque schema names: they can never shadow a handle verb or Object.prototype.
class HandleTreeFactory : public QObject {
    Q_OBJECT
public:
    explicit HandleTreeFactory(QJSEngine *engine) : QObject(engine) {
        create = engine->evaluate(QStringLiteral(R"JS((function(handle, names, kind) {
            const tree = Object.create(null);
            for (const name of names) {
                Object.defineProperty(tree, name, {enumerable:true, get:function() {
                    const child = handle.scope([name]);
                    if (kind !== "values")
                        return child;
                    return child.isField ? child.value : child.values;
                }});
            }
            return Object.freeze(tree);
        }))JS"));
    }
    QJSValue create;
};
QString typeName(ConfigDescriptor::Type type) {
    using T = ConfigDescriptor::Type;
    switch (type) {
    case T::Group:
        return "group";
    case T::Boolean:
        return "boolean";
    case T::Integer:
        return "integer";
    case T::UnsignedInteger:
        return "unsignedInteger";
    case T::Real:
        return "real";
    case T::String:
        return "string";
    case T::Url:
        return "url";
    case T::List:
        return "list";
    case T::Record:
        return "record";
    case T::Map:
        return "map";
    case T::SafeInteger:
        return "safeInteger";
    case T::SecretRef:
        return "secretRef";
    }
    return {};
}
} // namespace
ConfigQmlHandle::ConfigQmlHandle(ConfigHandle *handle, QJSEngine *engine, QObject *parent)
    : QObject(parent), m_handle(handle), m_engine(engine) {
    if (handle) {
        connect(handle, &ConfigHandle::valueChanged, this, &ConfigQmlHandle::valueChanged);
        connect(handle, &ConfigHandle::stateChanged, this, &ConfigQmlHandle::stateChanged);
        connect(handle, &QObject::destroyed, this, &ConfigQmlHandle::stateChanged);
    }
}
ConfigQmlHandle *ConfigQmlHandle::owned(ConfigHandle *handle, QJSEngine *engine, QObject *owner,
                                        const QString &name) {
    if (auto *existing = owner->findChild<ConfigQmlHandle *>(name, Qt::FindDirectChildrenOnly)) {
        if (existing->m_handle == handle)
            return existing;
        delete existing; // The owner rebound its native capability.
    }
    auto *result = new ConfigQmlHandle(handle, engine, owner);
    result->setObjectName(name);
    result->m_root = result;
    QQmlEngine::setObjectOwnership(result, QQmlEngine::CppOwnership);
    return result;
}
ConfigQmlHandle *ConfigQmlHandle::adopt(ConfigHandle *handle, QJSEngine *engine) {
    if (!handle)
        return nullptr;
    if (auto *existing = qobject_cast<ConfigQmlHandle *>(handle->parent()))
        return existing;
    // An opened edit root owns its session; its QML wrapper owns it in turn and
    // is reclaimed once no JS value (including a lookup) references it.
    auto *result = new ConfigQmlHandle(handle, engine, nullptr);
    handle->setParent(result);
    result->m_root = result;
    QQmlEngine::setObjectOwnership(result, QQmlEngine::JavaScriptOwnership);
    return result;
}
ConfigQmlHandle *ConfigQmlHandle::wrap(ConfigHandle *handle) {
    if (!handle || !m_engine)
        return nullptr;
    if (handle == m_handle)
        return this;
    auto *root = m_root ? m_root.data() : this;
    if (auto existing = root->m_wrappers.value(handle))
        return existing;
    // Lookups belong to their root wrapper, like native lookups belong to their
    // root handle: a binding that only observes a lookup (and holds no JS
    // reference) must keep receiving notifications, so lookups are never
    // garbage-collected on their own. An opened edit's lookups live as long as
    // the edit handle that owns the session.
    auto *result = new ConfigQmlHandle(handle, m_engine, root);
    result->m_root = root;
    QQmlEngine::setObjectOwnership(result, QQmlEngine::CppOwnership);
    root->m_wrappers.insert(handle, result);
    return result;
}
bool ConfigQmlHandle::valid() const { return m_handle && m_handle->valid(); }
QString ConfigQmlHandle::health() const {
    return m_handle ? m_handle->health() : QString("invalid");
}
QString ConfigQmlHandle::kind() const {
    if (!m_handle)
        return {};
    switch (m_handle->kind()) {
    case ConfigHandle::Kind::Live:
        return "live";
    case ConfigHandle::Kind::Staged:
        return "staged";
    case ConfigHandle::Kind::ReadOnly:
        return "readOnly";
    }
    return {};
}
QStringList ConfigQmlHandle::path() const { return m_handle ? m_handle->path() : QStringList(); }
bool ConfigQmlHandle::isField() const { return m_handle && m_handle->isField(); }
QStringList ConfigQmlHandle::capabilities() const {
    return m_handle ? m_handle->capabilities() : QStringList();
}
QJSValue ConfigQmlHandle::descriptor() const {
    if (!m_descriptor.isUndefined() || !m_engine || !m_handle)
        return m_descriptor;
    const auto descriptor = m_handle->descriptor();
    if (!descriptor)
        return m_descriptor = QJSValue(QJSValue::NullValue);
    // Immutable declared metadata only; never a widget, codec or current value.
    QVariantMap map{{"type", typeName(descriptor->type)},
                    {"optional", descriptor->optional},
                    {"nullable", descriptor->nullable},
                    {"sensitive", descriptor->sensitive},
                    {"hasDefault", descriptor->defaultValue.has_value()}};
    if (descriptor->defaultValue && !descriptor->sensitive)
        map.insert("default", qmlValue(*descriptor, *descriptor->defaultValue));
    if (!descriptor->choices.isEmpty())
        map.insert("choices", descriptor->choices);
    if (descriptor->minimum)
        map.insert("minimum", *descriptor->minimum);
    if (descriptor->maximum)
        map.insert("maximum", *descriptor->maximum);
    if (descriptor->type == ConfigDescriptor::Type::Group)
        map.insert("fields", QStringList(descriptor->fields.keys()));
    auto value = m_engine->toScriptValue(map);
    m_descriptor = m_engine->evaluate("Object.freeze").call({value});
    return m_descriptor;
}
QVariant ConfigQmlHandle::value() const {
    if (!m_handle || !m_handle->isField())
        return {};
    const auto native = m_handle->value();
    const auto descriptor = m_handle->descriptor();
    return descriptor && native.isValid() ? qmlValue(*descriptor, native) : QVariant();
}
bool ConfigQmlHandle::present() const { return m_handle && m_handle->present(); }
QJSValue ConfigQmlHandle::tree(const QString &kind) {
    if (!m_engine || !m_handle)
        return {};
    auto *factory = m_engine->findChild<HandleTreeFactory *>(QString(), Qt::FindDirectChildrenOnly);
    if (!factory)
        factory = new HandleTreeFactory(m_engine);
    QStringList names = m_handle->scopes();
    if (kind != "scopes")
        names += m_handle->fields();
    if (kind == "fields")
        names = m_handle->fields();
    return factory->create.call(
        {m_engine->newQObject(this), m_engine->toScriptValue(names), QJSValue(kind)});
}
// Lookup trees are cheap accessor plumbing over the cached native lookups. They
// are deliberately not retained in C++: a persistent tree would pin a JS-owned
// edit handle (and the session it owns) forever.
QJSValue ConfigQmlHandle::values() { return tree("values"); }
QJSValue ConfigQmlHandle::scopes() { return tree("scopes"); }
QJSValue ConfigQmlHandle::fields() { return tree("fields"); }
QJSValue ConfigQmlHandle::state() const {
    if (!m_handle || !m_engine)
        return {};
    return qmlSelectedStateEvent(m_engine, m_handle->state(), m_handle->descriptor());
}
QVariant ConfigQmlHandle::generation() const {
    return m_handle ? QVariant::fromValue(m_handle->generation()) : QVariant();
}
ConfigQmlHandle *ConfigQmlHandle::preview() {
    return m_handle ? wrap(m_handle->preview()) : nullptr;
}
ConfigQmlHandle *ConfigQmlHandle::scope(const QStringList &path) {
    return m_handle ? wrap(m_handle->scope(path)) : nullptr;
}
ConfigQmlHandle *ConfigQmlHandle::select(const QVariantList &paths) {
    if (!m_handle)
        return nullptr;
    QList<ConfigPath> selected;
    for (const auto &path : paths)
        selected.append(path.toStringList());
    return wrap(m_handle->select(selected));
}
ConfigQmlPendingReply *ConfigQmlHandle::reply(ConfigPendingReply *native) const {
    return m_engine ? new ConfigQmlPendingReply(native, m_engine) : (delete native, nullptr);
}
ConfigQmlPendingReply *ConfigQmlHandle::refuse(const QString &code, const QString &phase) const {
    return reply(ConfigPendingReplyState::refuse(code, phase));
}
bool ConfigQmlHandle::options(const QJSValue &value, ConfigHandleOptions &result) const {
    if (value.isUndefined() || value.isNull())
        return true;
    if (!value.isObject())
        return false;
    const auto observe = value.property("observe");
    if (observe.isUndefined())
        return true;
    if (!observe.isArray())
        return false;
    const auto length = observe.property("length").toUInt();
    for (quint32 i = 0; i < length; ++i) {
        auto *handle = qobject_cast<ConfigQmlHandle *>(observe.property(i).toQObject());
        if (!handle || !handle->m_handle)
            return false;
        result.observe.append(handle->m_handle);
    }
    return true;
}
ConfigQmlPendingReply *ConfigQmlHandle::read(const QJSValue &value) {
    ConfigHandleOptions selected;
    if (!m_handle)
        return refuse("read-owner", "lifetime");
    if (!options(value, selected))
        return refuse("read-observation", "selection");
    return reply(m_handle->read(selected));
}
ConfigQmlPendingReply *ConfigQmlHandle::set(const QJSValue &value, const QJSValue &extra) {
    ConfigHandleOptions selected;
    if (!m_handle)
        return refuse("read-owner", "lifetime");
    if (!options(extra, selected))
        return refuse("read-observation", "selection");
    QList<ConfigHandleEdit> batch;
    if (m_handle->isField())
        batch.append({{}, value.toVariant()});
    else {
        // A scope accepts only an explicit relative batch; omission is no edit.
        if (!value.isArray())
            return refuse("invalid-batch", "selection");
        const auto length = value.property("length").toUInt();
        for (quint32 i = 0; i < length; ++i) {
            const auto entry = value.property(i);
            const auto path = entry.property("path");
            if (!entry.isObject() || !path.isArray() || !entry.hasOwnProperty("value"))
                return refuse("invalid-batch", "selection");
            batch.append({path.toVariant().toStringList(), entry.property("value").toVariant()});
        }
    }
    return reply(ConfigHandleState::set(m_handle, batch, selected, ConfigDescriptor::Format::Qml));
}
#define CONFIGQML_HANDLE_VERB(name)                                                                \
    ConfigQmlPendingReply *ConfigQmlHandle::name(const QJSValue &value) {                          \
        ConfigHandleOptions selected;                                                              \
        if (!m_handle)                                                                             \
            return refuse("read-owner", "lifetime");                                               \
        if (!options(value, selected))                                                             \
            return refuse("read-observation", "selection");                                        \
        return reply(m_handle->name(selected));                                                    \
    }
CONFIGQML_HANDLE_VERB(remove)
CONFIGQML_HANDLE_VERB(unset)
CONFIGQML_HANDLE_VERB(apply)
CONFIGQML_HANDLE_VERB(discard)
CONFIGQML_HANDLE_VERB(edit)
CONFIGQML_HANDLE_VERB(abandon)
#undef CONFIGQML_HANDLE_VERB
ConfigQmlPendingReply *ConfigQmlHandle::observe(const QVariant &after, const QJSValue &extra) {
    ConfigHandleOptions selected;
    if (!m_handle)
        return refuse("read-owner", "lifetime");
    if (!options(extra, selected))
        return refuse("read-observation", "selection");
    ConfigObservation token;
    if (after.isValid() && !after.isNull()) {
        // Tokens are library-owned values; a forged JS object is not a token.
        if (after.metaType() != QMetaType::fromType<ConfigObservation>())
            return refuse("read-observation", "observation");
        token = after.value<ConfigObservation>();
    }
    return reply(m_handle->observe(token, selected));
}
ConfigQmlPendingReply *ConfigQmlHandle::whenCurrent(const QVariant &after, const QJSValue &extra) {
    return observe(after, extra);
}
ConfigQmlHandle *ConfigQmlStore::root() {
    ConfigHandle *native = nullptr;
    if (m_overlay)
        native = m_overlay->root();
    else if (m_draft)
        native = m_isPreview ? ConfigHandleState::root(m_draft, true) : m_draft->root();
    else if (m_nativeScope || (m_core && m_isHandle)) {
        if (!m_nativeScope && m_core)
            m_nativeScope = m_core->scope(m_prefix, m_writable, this);
        native = m_nativeScope ? m_nativeScope->root() : nullptr;
    } else if (m_core)
        native = m_core->root();
    return native
               ? ConfigQmlHandle::owned(native, scriptEngine(), this, "_configqml_qml_root_handle")
               : nullptr;
}
ConfigQmlHandle *ConfigQmlStore::runtimeRoot() {
    if (!m_core || m_isHandle || m_draft || m_overlay)
        return nullptr;
    return ConfigQmlHandle::owned(m_core->runtimeRoot(), scriptEngine(), this,
                                  "_configqml_qml_runtime_root_handle");
}
ConfigQmlHandle *ConfigQmlOverlay::root() { return m_adapter->root(); }
} // namespace configqml
#include "configqmlhandle.moc"
