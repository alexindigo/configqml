// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configqmlvalue.h"
#include "configconsumer_p.h"
#include "configstorehandle.h"
#include <QQmlEngine>

namespace configqml {
bool ConfigQmlStore::canWrite(const ConfigPath &path) const {
    if (m_overlay)
        return m_overlay->writable() && m_overlay->schema().descriptor(path).has_value();
    return m_core && m_writable && startsWith(path, m_prefix) && m_core->canWriteUser(path) &&
           (!m_draft || m_draft->paths().contains(path));
}
void ConfigQmlStore::initializeHandle(ConfigStore *core, const ConfigPath &prefix, bool writable,
                                      ConfigDraft *draft, bool preview) {
    m_complete = true;
    m_isHandle = true;
    m_core = core;
    m_diagnostics = core->schema().diagnostics();
    m_prefix = ownPath(prefix);
    m_writable = writable && !preview;
    m_draft = preview ? nullptr : draft;
    m_previewDraft = preview ? draft : nullptr;
    m_isPreview = preview;
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    if (preview && draft) {
        connect(draft, &ConfigDraft::previewValuesChanged, this,
                &ConfigQmlStore::settingValuesChanged);
        connect(draft, &QObject::destroyed, this, &ConfigQmlStore::statusChanged);
    } else if (draft) {
        connect(draft, &ConfigDraft::valuesChanged, this, &ConfigQmlStore::settingValuesChanged);
        connect(draft, &ConfigDraft::stateChanged, this, &ConfigQmlStore::draftStateChanged);
    } else
        connect(core, &ConfigStore::valuesChanged, this, &ConfigQmlStore::settingValuesChanged);
    connect(core, &ConfigStore::statusChanged, this, &ConfigQmlStore::statusChanged);
    connect(core, &ConfigStore::completed, this, [this](const ConfigResult &result) {
        ConfigConsumerGuard(this).invoke([&] { emit completed(qmlResult(result, m_diagnostics)); });
    });
    const auto schema = preview ? draft->previewSchema() : core->schema();
    m_values = createConfigNamespace(this, *schema.descriptor(prefix), prefix, this);
    initializeOverlaySignals();
}
ConfigQmlStore *ConfigQmlStore::scope(const QStringList &key, bool writable) {
    if (!m_core || m_draft) {
        reject(m_prefix + key, "detached-handle");
        return nullptr;
    }
    const auto path = m_prefix + key;
    const auto descriptor = m_core->schema().descriptor(path);
    if (!descriptor || descriptor->type != ConfigDescriptor::Type::Group) {
        reject(m_prefix + key, "undeclared-scope");
        return nullptr;
    }
    auto *handle = new ConfigQmlStore(this);
    if (m_isPreview) {
        auto *scope = m_nativeScope ? m_nativeScope->scope(key, false) : nullptr;
        if (!scope) {
            delete handle;
            reject(path, "detached-handle");
            return nullptr;
        }
        handle->initializeHandle(m_core, path, false, m_previewDraft, true);
        scope->setParent(handle);
        handle->m_nativeScope = scope;
        connect(scope, &ConfigStoreHandle::detached, handle, &ConfigQmlStore::statusChanged);
    } else
        handle->initializeHandle(m_core, path, writable && m_writable);
    return handle;
}
QObject *ConfigQmlStore::preview() {
    if (!m_core || !m_draft)
        return nullptr;
    if (!m_previewAdapter) {
        auto *view = new ConfigQmlStore(this);
        view->m_callbackContext = this; // The hidden adapter itself has no JS wrapper.
        view->initializeHandle(m_core, m_prefix, false, m_draft, true);
        m_previewAdapter = view;
    }
    return m_previewAdapter->values();
}
ConfigQmlStore *ConfigQmlStore::previewScope(const QStringList &relative) {
    if (!m_core || !m_draft) {
        reject(relative, "not-a-draft");
        return nullptr;
    }
    const auto path = m_prefix + relative;
    auto *scope = m_draft->previewScope(path);
    if (!scope) {
        reject(path, "undeclared-scope");
        return nullptr;
    }
    auto *view = new ConfigQmlStore(this);
    view->initializeHandle(m_core, path, false, m_draft, true);
    scope->setParent(view);
    view->m_nativeScope = scope;
    connect(scope, &ConfigStoreHandle::detached, view, &ConfigQmlStore::statusChanged);
    return view;
}
QList<ConfigPath> ConfigQmlStore::selectedPaths(const QVariantList &paths, bool *ok) {
    const auto prefix = m_prefix;
    const auto captured = captureQmlValue(paths, {}, m_diagnostics);
    *ok = captured.ok();
    QList<ConfigPath> result;
    for (const auto &item : captured.value.toList()) {
        if (item.metaType().id() != QMetaType::QVariantList &&
            item.metaType().id() != QMetaType::QStringList) {
            *ok = false;
            break;
        }
        ConfigPath path = prefix;
        for (const auto &segment : item.toList()) {
            if (segment.metaType().id() != QMetaType::QString) {
                *ok = false;
                break;
            }
            path.append(segment.toString());
        }
        result.append(path);
    }
    if (result.isEmpty())
        result.append(prefix);
    return result;
}
ConfigQmlStore *ConfigQmlStore::draft(const QVariantList &paths) {
    if (!m_core || !m_writable || m_draft) {
        reject({}, "detached-or-read-only");
        return nullptr;
    }
    bool ok = false;
    QList<ConfigPath> selection;
    ConfigConsumerGuard boundary(this);
    if (!boundary.invoke([&] { selection = selectedPaths(paths, &ok); }))
        return nullptr;
    if (!m_core)
        return nullptr;
    if (!ok) {
        reject({}, "draft-paths");
        return nullptr;
    }
    auto *editor = m_core->draft(selection, this);
    if (!editor->valid()) {
        const auto code = editor->error();
        delete editor;
        reject({}, code);
        return nullptr;
    }
    auto *handle = new ConfigQmlStore(this);
    editor->setParent(handle);
    handle->initializeHandle(m_core, m_prefix, true, editor);
    return handle;
}
QString ConfigQmlStore::apply(const QJSValue &function) {
    if (m_overlay)
        return QString::number(m_overlay->apply(this, callback(function)));
    if (!m_draft || !m_core || m_draft->busy()) {
        reject({}, "not-an-idle-draft");
        return {};
    }
    quint64 id = 0;
    ConfigConsumerGuard(this).invoke([&] { id = m_draft->apply(this, callback(function)); });
    return QString::number(id);
}
bool ConfigQmlStore::discard() {
    if (!m_draft || !m_core)
        return false;
    bool result = false;
    ConfigConsumerGuard(this).invoke([&] { result = m_draft->discard(); });
    return result;
}
void ConfigQmlStore::detach() {
    if (!m_isHandle) {
        reject({}, "not-a-scoped-handle");
        return;
    }
    if (m_core)
        disconnect(m_core, nullptr, this, nullptr);
    if (m_nativeScope)
        m_nativeScope->detach();
    if (m_overlay)
        m_overlay->detach();
    m_core = nullptr;
    m_draft = nullptr;
    ConfigConsumerGuard(this).invoke([&] { emit statusChanged(); });
}
QString ConfigQmlStore::exportProfile(const QString &name, const QString &file,
                                      const QVariantList &paths, const QJSValue &function) {
    if (!m_core || m_draft) {
        reject({}, "detached-handle");
        return {};
    }
    bool ok = false;
    QList<ConfigPath> selection;
    ConfigConsumerGuard boundary(this);
    if (!boundary.invoke([&] { selection = selectedPaths(paths, &ok); }))
        return {};
    if (!m_core)
        return {};
    if (!ok) {
        reject({}, "profile-paths");
        return {};
    }
    return QString::number(m_core->exportProfile(name, file, selection, this, callback(function)));
}
QString ConfigQmlStore::importProfile(const QString &file, const QVariantList &paths,
                                      const QJSValue &function) {
    if (!m_core || !m_writable || m_draft) {
        reject({}, "detached-or-read-only");
        return {};
    }
    bool ok = false;
    QList<ConfigPath> selection;
    ConfigConsumerGuard boundary(this);
    if (!boundary.invoke([&] { selection = selectedPaths(paths, &ok); }))
        return {};
    if (!m_core)
        return {};
    if (!ok) {
        reject({}, "profile-paths");
        return {};
    }
    return QString::number(m_core->importProfile(file, selection, this, callback(function)));
}
} // namespace configqml
