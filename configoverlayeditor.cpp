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
    s.entryPath = ownValueSelectors(options.entryPath);
    s.identityPath = ownValueSelectors(options.identityPath);
    s.overrideKey = ownString(options.overrideKey);
    for (auto *selectors : {&s.entryPath, &s.identityPath})
        for (const auto &selector : *selectors) {
            if ((selector.kind != ConfigValueSelector::Kind::Key &&
                 selector.kind != ConfigValueSelector::Kind::Index) ||
                selector.index < 0)
                t->error = "overlay-selection";
        }
    const auto identity = ownValue(options.identityValue);
    if (!identity.ok())
        t->error = "overlay-identity";
    s.identityValue = identity.value;
    if (s.identityPath.isEmpty() || s.overrideKey.isEmpty())
        t->error = "overlay-options";
    if ((options.mode != ConfigOverlayMode::Editor && options.mode != ConfigOverlayMode::View &&
         options.mode != ConfigOverlayMode::Live) ||
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
    if (m_mode == ConfigOverlayMode::Live)
        connect(store, &ConfigStore::statusChanged, this, [this] {
            if (m_store && m_store->status() != ConfigStore::Status::Ready) {
                ConfigOverlayPresentation state;
                state.error = "overlay-owner";
                install(state);
            }
        });
}
ConfigOverlayEditor::~ConfigOverlayEditor() { detach(); }
bool ConfigOverlayEditor::dirty() const {
    return !m_edits.isEmpty() || m_pendingReset || m_liveDirty;
}
QVariant ConfigOverlayEditor::value(const ConfigPath &path) const {
    const auto edit = m_edits.constFind(path);
    return edit == m_edits.cend()              ? m_values.value(path)
           : edit.value()                      ? *edit.value()
           : m_mode == ConfigOverlayMode::Live ? m_baseValues.value(path)
                                               : QVariant();
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
    if (m_mode == ConfigOverlayMode::Live)
        return writeLive(ownPath(path), checked.value);
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
    if (m_mode == ConfigOverlayMode::Live)
        return writeLive(path, std::nullopt).accepted;
    m_edits[path] = std::nullopt;
    m_presentations.remove(path);
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged({path}); }))
        boundary.invoke([&] { emit stateChanged(); });
    return true;
}
ConfigRequestHandle ConfigOverlayEditor::writeLive(const ConfigPath &path,
                                                   const std::optional<QVariant> &value) {
    if (!m_store || !m_store->m_accepting)
        return {0, 0, false, {m_schema.diagnostics().make(path, "overlay-owner")}};
    QPointer<ConfigOverlayEditor> guard(this);
    const auto id = m_store->overlayRequest(this, "overlayWrite", this,
                                            [guard](const ConfigResult &result) {
                                                if (guard && guard->status() != Status::Closed)
                                                    ConfigConsumerGuard(guard).invoke(
                                                        [&] { emit guard->completed(result); });
                                            },
                                            {{path, value}});
    const auto sequence = m_store->m_sequence;
    m_edits[path] = value;
    m_editSequences[path] = sequence;
    m_presentations.remove(path);
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged({path}); }))
        boundary.invoke([&] { emit stateChanged(); });
    return {id, sequence, true, {}};
}
void ConfigOverlayEditor::install(const ConfigOverlayPresentation &p) {
    if (m_status == Status::Closed)
        return;
    if (p.dirtyKnown)
        m_liveDirty = p.dirty;
    m_conflicted |= p.conflicted;
    if (!p.error.isEmpty()) {
        m_error = p.error;
        m_status = Status::Error;
    } else if (p.refreshed && !(m_mode == ConfigOverlayMode::Live && m_status == Status::Error)) {
        m_values = p.values;
        m_baseValues = p.baseValues;
        m_presentations.clear();
        if (m_mode == ConfigOverlayMode::Live) {
            for (auto it = m_editSequences.begin(); it != m_editSequences.end();) {
                if (it.value() <= p.coveredSequence) {
                    m_edits.remove(it.key());
                    it = m_editSequences.erase(it);
                } else
                    ++it;
            }
        } else
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
                                    QObject *context, Callback callback,
                                    const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    const auto id = registerRequest(context, std::move(callback));
    ConfigCommand command{id, m_sequence, operation, {}};
    command.editorId = editor->m_id;
    command.overlayEdits = operation == "overlayWrite" ? edits : editor->m_edits;
    if (!editor->writable() || editor->busy() || !editor->ready())
        command.conversionError = !editor->writable() ? "overlay-readonly"
                                  : editor->busy()    ? "overlay-busy"
                                                      : "overlay-owner";
    const bool liveOperation =
        operation == "overlayWrite" || operation == "save" || operation == "flush";
    if (command.conversionError.isEmpty() &&
        ((editor->m_mode == ConfigOverlayMode::Live) != liveOperation))
        command.conversionError = "overlay-readonly";
    const bool changes =
        command.conversionError.isEmpty() &&
        (operation == "overlayWrite" || operation == "overlayApply" || operation == "overlayReset");
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
    if (!m_accepting || (liveOperation && !command.conversionError.isEmpty()) ||
        !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = operation;
        result.editorId = command.editorId;
        result.status = "failed";
        result.refreshState = "not_refreshed";
        result.error = {command.conversionError.isEmpty() ? QString("overlay-owner")
                                                          : command.conversionError,
                        {},
                        operation};
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
    const bool active = m_mode == ConfigOverlayMode::Editor && writable() && !busy() && ready();
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
            if ((active && !boundary.invoke([&] { emit guard->stateChanged(); })) ||
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
    if (active)
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
quint64 ConfigOverlayEditor::save(QObject *context, ConfigStore::Callback callback) {
    return request("save", context, std::move(callback));
}
quint64 ConfigOverlayEditor::flush(QObject *context, ConfigStore::Callback callback) {
    return request("flush", context, std::move(callback));
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
