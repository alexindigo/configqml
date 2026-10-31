// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configqmloverlay.h"
#include "configqmlvalue.h"
#include "configstorehandle.h"
#include "configconsumer_p.h"
#include "configoverlay_p.h"
#include <QQmlEngine>
#include <QJSValueIterator>
#include <cmath>

namespace configqml {
QVariantMap ConfigQmlStore::resultMap(const ConfigResult &result) const {
    auto map = qmlResult(result, m_diagnostics);
    if (result.status == "opened" && m_overlayHandles.value(result.editorId))
        map["editor"] = QVariant::fromValue<QObject *>(m_overlayHandles.value(result.editorId));
    return map;
}
void ConfigQmlStore::initializeOverlaySignals() {
    connect(
        m_core, &ConfigStore::overlayOpened, this, [this](quint64 id, ConfigOverlayEditor *editor) {
            if (m_overlayRequests.remove(id))
                m_overlayHandles[editor->editorId()] = new ConfigQmlOverlay(m_core, editor, this);
        });
}
ConfigQmlOverlay::ConfigQmlOverlay(ConfigStore *store, ConfigOverlayEditor *editor, QObject *parent)
    : QObject(parent), m_adapter(new ConfigQmlStore(this)) {
    m_adapter->m_complete = m_adapter->m_isHandle = true;
    m_adapter->m_core = store;
    m_adapter->m_overlay = editor;
    m_adapter->m_callbackContext = this;
    m_adapter->m_diagnostics = editor->schema().diagnostics();
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    connect(editor, &ConfigOverlayEditor::valuesChanged, m_adapter,
            &ConfigQmlStore::settingValuesChanged);
    connect(editor, &ConfigOverlayEditor::stateChanged, this, &ConfigQmlOverlay::stateChanged);
    connect(editor, &ConfigOverlayEditor::completed, this, [this](const ConfigResult &result) {
        ConfigConsumerGuard(this).invoke([&] { emit completed(m_adapter->resultMap(result)); });
    });
    connect(m_adapter, &ConfigQmlStore::inputRejected, this, &ConfigQmlOverlay::inputRejected);
    m_adapter->m_values =
        createConfigNamespace(m_adapter, *editor->schema().descriptor({}), {}, m_adapter);
}
ConfigQmlOverlay::~ConfigQmlOverlay() {
    if (m_adapter->m_overlay)
        m_adapter->m_overlay->detach();
}
QString ConfigQmlOverlay::apply(const QJSValue &callback) { return m_adapter->apply(callback); }
QString ConfigQmlOverlay::resetOverrides(const QJSValue &callback) {
    return m_adapter->resetOverrides(callback);
}
QString ConfigQmlOverlay::discard(const QJSValue &callback) {
    return m_adapter->m_overlay ? QString::number(m_adapter->m_overlay->discard(
                                      m_adapter, m_adapter->callback(callback)))
                                : QString();
}
bool ConfigQmlOverlay::unset(const QStringList &path) { return m_adapter->unset(path); }
void ConfigQmlOverlay::detach() { m_adapter->detach(); }
QString ConfigQmlStore::openOverlay(const QJSValue &options, const QJSValue &function) {
    ConfigConsumerGuard boundary(this);
    if (!m_core || m_draft || m_overlay) {
        reject({}, "overlay-selection");
        return {};
    }
    auto *engine = qjsEngine(this);
    if (!engine || !options.isObject() || options.isArray() || options.isCallable() ||
        options.isQObject() || options.isQMetaObject() || options.isDate() || options.isRegExp() ||
        options.isError()) {
        reject({}, "overlay-options");
        return {};
    }
    ConfigOverlayOptions native;
    QPointer<ConfigQmlStore> base;
    QVariantMap captured;
    QString error;
    bool semanticError = false;
    if (!boundary.invoke([&] {
            auto plain = engine->newObject();
            plain.setPrototype(QJSValue(QJSValue::NullValue));
            QJSValueIterator iterator(options);
            while (iterator.hasNext()) {
                iterator.next();
                if (iterator.name() == "base")
                    base = qobject_cast<ConfigQmlStore *>(iterator.value().toQObject());
                else
                    plain.setProperty(iterator.name(), iterator.value());
            }
            const auto result = captureQmlValue(QVariant::fromValue(plain), {}, m_diagnostics);
            if (!result.ok())
                error = result.errors.first().code;
            captured = result.value.toMap();
        }))
        return {};
    const QSet<QString> allowed{"mode",        "ownerPath",    "ownerGeneration",
                                "entryPath",   "identityPath", "identityValue",
                                "overrideKey", "mergePolicy",  "differencePolicy"};
    for (auto it = captured.cbegin(); it != captured.cend(); ++it)
        if (!allowed.contains(it.key()))
            semanticError = true;
    for (const auto &key : allowed)
        if (key != "mode" && !captured.contains(key))
            error = "overlay-options";
    const auto mode = captured.value("mode", "editor");
    if (mode.metaType().id() != QMetaType::QString)
        error = "overlay-options";
    else if (mode != "editor" && mode != "view")
        semanticError = true;
    native.mode = mode == "view" ? ConfigOverlayMode::View : ConfigOverlayMode::Editor;
    if (captured.value("mergePolicy") != "recursive_objects_replace_lists" ||
        captured.value("differencePolicy") != "top_level_omit_equal")
        semanticError = true;
    const auto path = captured.value("ownerPath");
    if (path.metaType().id() != QMetaType::QVariantList)
        error = "overlay-options";
    for (const auto &segment : path.toList()) {
        if (segment.metaType().id() != QMetaType::QString)
            error = "overlay-options";
        native.ownerPath.append(segment.toString());
    }
    native.ownerPath = m_prefix + native.ownerPath;
    const auto generation = captured.value("ownerGeneration");
    bool validGeneration = false;
    native.ownerGeneration = generation.toString().toULongLong(&validGeneration);
    if (generation.metaType().id() != QMetaType::QString || !validGeneration ||
        QString::number(native.ownerGeneration) != generation.toString())
        error = "overlay-options";
    if (!qmlSelectors(captured.value("entryPath"), native.entryPath) ||
        !qmlSelectors(captured.value("identityPath"), native.identityPath))
        error = "overlay-options";
    native.identityValue = captured.value("identityValue");
    const auto ownerDescriptor = m_core->schema().descriptor(native.ownerPath);
    const auto rowDescriptor =
        ownerDescriptor ? overlayDescriptor(*ownerDescriptor, native.entryPath) : std::nullopt;
    const auto identityDescriptor =
        rowDescriptor ? overlayDescriptor(*rowDescriptor, native.identityPath) : std::nullopt;
    if (identityDescriptor) {
        const auto checked = admitQmlValue(*identityDescriptor, native.identityValue);
        if (!checked.ok())
            error = checked.errors.first().code;
        else
            native.identityValue = checked.value;
    }
    if (captured.value("overrideKey").metaType().id() != QMetaType::QString)
        error = "overlay-options";
    native.overrideKey = captured.value("overrideKey").toString();
    if (!error.isEmpty()) {
        reject({}, error);
        return {};
    }
    if (semanticError)
        native.mergePolicy = static_cast<ConfigOverlayMergePolicy>(-1);
    if (base && base->m_core && base->m_isHandle && !base->m_draft && !base->m_overlay) {
        if (!base->m_nativeScope)
            base->m_nativeScope = base->m_core->scope(base->m_prefix, base->m_writable, base);
        native.base = base->m_nativeScope;
    }
    quint64 id = 0;
    if (boundary.invoke([&] { id = m_core->openOverlay(native, this, callback(function)); }))
        m_overlayRequests.insert(id);
    return QString::number(id);
}
QString ConfigQmlStore::resetOverrides(const QJSValue &function) {
    if (!m_overlay) {
        reject({}, "overlay-selection");
        return {};
    }
    return QString::number(m_overlay->resetOverrides(this, callback(function)));
}
bool ConfigQmlStore::unset(const QStringList &path) {
    if (!m_overlay || !m_overlay->unset(path)) {
        reject(path, m_overlay && !m_overlay->writable() ? "overlay-readonly"
                     : m_overlay && m_overlay->busy()    ? "overlay-busy"
                                                         : "overlay-selection");
        return false;
    }
    return true;
}
} // namespace configqml
