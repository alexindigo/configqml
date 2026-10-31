// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configoverlayeditor.h"
#include "configoverlay_p.h"
#include "configshutdown_p.h"
#include "configstorehandle.h"
#include "configconsumer_p.h"
#include <QMutexLocker>
#include <QUuid>

namespace configqml {
namespace {
QMutex graphMutex;
bool reaches(const std::shared_ptr<ConfigOverlayEndpoint> &from,
             const std::shared_ptr<ConfigOverlayEndpoint> &target, QSet<quint64> &seen) {
    if (from == target)
        return true;
    if (seen.contains(from->order))
        return false;
    seen.insert(from->order);
    for (const auto &weak : from->dependencies)
        if (auto next = weak.lock())
            if (next->live && reaches(next, target, seen))
                return true;
    return false;
}
} // namespace
void overlayRemoveDependency(const std::shared_ptr<ConfigOverlayEndpoint> &endpoint,
                             const QString &id) {
    QMutexLocker lock(&graphMutex);
    endpoint->dependencies.remove(id);
}
quint64 ConfigStore::openOverlay(const ConfigOverlayOptions &options, QObject *context,
                                 Callback callback) {
    const auto id = registerRequest(context, std::move(callback));
    auto t = std::make_shared<ConfigOverlayTransaction>();
    t->editorId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto &s = t->selection;
    s.mode = options.mode;
    s.owner = m_owner->overlayEndpoint();
    s.ownerPath = ownPath(options.ownerPath);
    s.ownerGeneration = options.ownerGeneration;
    s.entryPath = options.entryPath;
    s.identityPath = options.identityPath;
    s.overrideKey = ownString(options.overrideKey);
    for (auto *selectors : {&s.entryPath, &s.identityPath})
        for (auto &selector : *selectors) {
            selector.key = ownString(selector.key);
            if (selector.kind == ConfigValueSelector::Kind::Each || selector.index < 0)
                t->error = "overlay-selection";
        }
    const auto identity = ownValue(options.identityValue);
    if (!identity.ok())
        t->error = "overlay-identity";
    s.identityValue = identity.value;
    if (s.identityPath.isEmpty() || s.overrideKey.isEmpty())
        t->error = "overlay-options";
    if ((options.mode != ConfigOverlayMode::Editor && options.mode != ConfigOverlayMode::View) ||
        options.mergePolicy != ConfigOverlayMergePolicy::RecursiveObjectsReplaceLists ||
        options.differencePolicy != ConfigOverlayDifferencePolicy::TopLevelOmitEqual)
        t->error = "overlay-options";
    if (options.base && options.base->thread() == thread() && options.base->m_store) {
        s.base = options.base->m_store->m_owner->overlayEndpoint();
        s.basePath = options.base->m_prefix;
        s.baseLifetime = options.base->m_lifetime;
        for (const auto &path : options.base->m_store->schema().leaves())
            if (startsWith(path, s.basePath) && options.base->m_store->info(path).latestInput >
                                                    options.base->m_store->info(path).workerCovered)
                t->error = "unsettled-overlay-base";
    } else
        t->error = "overlay-selection";
    if (t->error.isEmpty()) {
        QMutexLocker lock(&graphMutex);
        QSet<quint64> seen;
        if (reaches(s.base, s.owner, seen))
            t->error = "overlay-cycle";
        else
            s.owner->dependencies[t->editorId] = s.base;
    }
    ConfigCommand command{id, m_sequence, "openOverlay", {}};
    command.overlay = t;
    if (!m_accepting || !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = "openOverlay";
        result.status = "failed";
        result.editorId = t->editorId;
        result.error = {"store-closed", {}, command.operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    }
    return id;
}
ConfigOverlayEditor::ConfigOverlayEditor(ConfigStore *store, const ConfigOverlayPresentation &p)
    : QObject(store), m_store(store), m_id(p.editorId), m_schema(p.schema), m_mode(p.mode) {
    m_ownerPath = p.ownerPath;
    install(p);
}
ConfigOverlayEditor::~ConfigOverlayEditor() { detach(); }
QVariant ConfigOverlayEditor::value(const ConfigPath &path) const {
    const auto edit = m_edits.constFind(path);
    return edit == m_edits.cend() ? m_values.value(path) : edit.value().value_or(QVariant());
}
QVariant ConfigOverlayEditor::presentation(const ConfigPath &path) const {
    if (!m_presentations.contains(path)) {
        const auto descriptor = m_schema.descriptor(path);
        m_presentations[ownPath(path)] =
            descriptor ? collectionPresentation(*descriptor, value(path)) : QVariant();
    }
    return m_presentations.value(path);
}
ConfigRequestHandle ConfigOverlayEditor::setValue(const ConfigPath &path, const QVariant &value) {
    const auto checked = m_schema.validate(path, value);
    ConfigRequestHandle result;
    result.errors = checked.errors;
    if (result.errors.isEmpty() && (!writable() || !ready() || m_busy))
        result.errors = {m_schema.diagnostics().make(path, !writable() ? "overlay-readonly"
                                                           : m_busy    ? "overlay-busy"
                                                                       : "overlay-owner")};
    if (!result.errors.isEmpty())
        return result;
    m_edits[ownPath(path)] = checked.value;
    m_presentations.remove(path);
    result.accepted = true;
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged({path}); }))
        boundary.invoke([&] { emit stateChanged(); });
    return result;
}
ConfigRequestHandle ConfigOverlayEditor::setValue(const ConfigAdmission &admission) {
    const auto checked = m_schema.admitted(admission);
    if (!checked.ok())
        return {0, 0, false, checked.errors};
    return setValue(admission.path(), checked.value);
}
bool ConfigOverlayEditor::unset(const ConfigPath &query) {
    const auto path = ownPath(query);
    const auto descriptor = m_schema.descriptor(path);
    if (!writable() || !ready() || m_busy || !descriptor ||
        descriptor->type == ConfigDescriptor::Type::Group)
        return false;
    m_edits[path] = std::nullopt;
    m_presentations.remove(path);
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged({path}); }))
        boundary.invoke([&] { emit stateChanged(); });
    return true;
}
void ConfigOverlayEditor::install(const ConfigOverlayPresentation &p) {
    if (m_status == Status::Closed)
        return;
    m_conflicted |= p.conflicted;
    if (!p.error.isEmpty()) {
        m_error = p.error;
        m_status = Status::Error;
    } else if (p.refreshed) {
        m_values = p.values;
        m_presentations.clear();
        m_edits.clear();
        m_pendingReset = false;
        m_conflicted = false;
        m_error.clear();
        m_status = Status::Ready;
    }
    ConfigConsumerGuard boundary(this);
    if (p.refreshed && !boundary.invoke([&] { emit valuesChanged(m_schema.leaves()); }))
        return;
    boundary.invoke([&] { emit stateChanged(); });
}
void ConfigStore::receiveOverlay(const ConfigOverlayPresentation &p) {
    if (auto editor = m_overlays.value(p.editorId))
        editor->install(p);
}
quint64 ConfigStore::overlayRequest(ConfigOverlayEditor *editor, const QString &operation,
                                    QObject *context, Callback callback) {
    const auto id = registerRequest(context, std::move(callback));
    ConfigCommand command{id, m_sequence, operation, {}};
    command.editorId = editor->m_id;
    command.overlayEdits = editor->m_edits;
    if (!editor->writable() || editor->busy() || !editor->ready())
        command.conversionError = !editor->writable() ? "overlay-readonly"
                                  : editor->busy()    ? "overlay-busy"
                                                      : "overlay-owner";
    const bool changes = command.conversionError.isEmpty() && operation != "overlayDiscard";
    if (changes) {
        ConfigInput input;
        input.kind = ConfigInput::Kind::Group;
        input.sequence = m_sequence + 1;
        // The worker supplies the replacement. Only the selected owning-leaf ingress is tracked
        // here.
        const auto path = editor->m_ownerPath;
        input.pathGenerations[path] = info(path).pathGeneration + 1;
        command.input = input;
        command.prefix = input.sequence;
    }
    if (!m_accepting || !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = operation;
        result.editorId = command.editorId;
        result.status = "failed";
        result.refreshState = "not_refreshed";
        result.error = {"overlay-owner", {}, operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    } else if (changes) {
        m_sequence = command.prefix;
        for (auto it = command.input->pathGenerations.cbegin();
             it != command.input->pathGenerations.cend(); ++it) {
            m_cache.information[it.key()].latestInput = command.prefix;
            m_cache.information[it.key()].pathGeneration = it.value();
        }
    }
    return id;
}
quint64 ConfigOverlayEditor::request(const QString &operation, QObject *context,
                                     ConfigStore::Callback callback) {
    if (!m_store)
        return 0;
    const bool active = writable() && !busy() && ready();
    QPointer<ConfigOverlayEditor> guard(this);
    QPointer<QObject> target(context ? context : this);
    const auto id = m_store->overlayRequest(
        this, operation, this,
        [guard, target, active, callback = std::move(callback)](const ConfigResult &result) {
            if (!guard || guard->status() == Status::Closed)
                return;
            if (active)
                guard->m_busy = false;
            if (result.error.code == "overlay-conflict")
                guard->m_conflicted = true;
            ConfigConsumerGuard boundary(guard);
            if (!boundary.invoke([&] { emit guard->stateChanged(); }) ||
                !boundary.invoke([&] { emit guard->completed(result); }))
                return;
            if (target && callback) {
                if (target->thread() == QThread::currentThread())
                    boundary.invoke([&] { callback(result); });
                else
                    QMetaObject::invokeMethod(
                        target,
                        [target, callback, result] {
                            ConfigConsumerGuard(target).invoke([&] { callback(result); });
                        },
                        Qt::QueuedConnection);
            }
        });
    if (active)
        m_busy = true;
    if (active && operation == "overlayReset")
        m_pendingReset = true;
    ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}
quint64 ConfigOverlayEditor::apply(QObject *context, ConfigStore::Callback callback) {
    return request("overlayApply", context, std::move(callback));
}
quint64 ConfigOverlayEditor::resetOverrides(QObject *context, ConfigStore::Callback callback) {
    return request("overlayReset", context, std::move(callback));
}
quint64 ConfigOverlayEditor::discard(QObject *context, ConfigStore::Callback callback) {
    return request("overlayDiscard", context, std::move(callback));
}
void ConfigOverlayEditor::detach() {
    if (m_status == Status::Closed)
        return;
    m_status = Status::Closed;
    if (m_store) {
        const auto endpoint = m_store->m_owner->overlayEndpoint();
        overlayRemoveDependency(endpoint, m_id);
        endpoint->send([id = m_id](ConfigWorker &worker) { worker.overlayDetach(id); });
    }
    m_store = nullptr;
    ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
}
} // namespace configqml
