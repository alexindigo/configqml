// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configconsumer_p.h"
#include "configprojection_p.h"
#include "configdraft_p.h"
#include "configapply_p.h"
#include "configchange_p.h"
#include "configstorehandle.h"
#include "configshutdown_p.h"
#include "configreadprojection_p.h"
#include <QUuid>

namespace configqml {
namespace {
ConfigDescriptor previewDescriptor(ConfigDescriptor descriptor, const ConfigPath &prefix,
                                   const QList<ConfigPath> &paths) {
    for (auto it = descriptor.fields.begin(); it != descriptor.fields.end();) {
        const auto path = prefix + ConfigPath{it.key()};
        if (it->type == ConfigDescriptor::Type::Group) {
            it.value() = previewDescriptor(it.value(), path, paths);
            if (!it->fields.isEmpty()) {
                ++it;
                continue;
            }
        } else if (paths.contains(path)) {
            ++it;
            continue;
        }
        it = descriptor.fields.erase(it);
    }
    return descriptor;
}
} // namespace
ConfigSchema draftPreviewSchema(const ConfigSchema &schema, const QList<ConfigPath> &paths) {
    return ConfigSchema(previewDescriptor(*schema.descriptor({}), {}, paths));
}
ConfigDraft::ConfigDraft(ConfigStore *store, const QList<ConfigPath> &paths, QObject *parent)
    : QObject(parent), m_store(store), m_schema(store->schema()) {
    m_changes = std::make_unique<ConfigChangePublisher>();
    m_previewChanges = std::make_unique<ConfigChangePublisher>();
    new ConfigDraftState(this);
    store->m_changes->add(this, [this](const auto &) { prepareStoreChange(); });
    if (store->status() != ConfigStore::Status::Ready) {
        m_error = "not-ready";
        return;
    }
    for (const auto &selection : paths)
        if (!m_schema.descriptor(selection)) {
            m_error = "undeclared-path";
            return;
        }
    for (const auto &path : m_schema.leaves()) {
        bool selected = paths.isEmpty();
        for (const auto &prefix : paths)
            selected |= startsWith(path, prefix);
        if (!selected || !store->canWriteUser(path))
            continue;
        if (store->info(path).latestInput > store->info(path).workerCovered) {
            m_error = "unsettled-draft-base";
            return;
        }
        m_base[path] = store->value(path);
    }
    if (m_base.isEmpty())
        m_error = "empty-draft-scope";
    refresh();
    m_previewSchema = draftPreviewSchema(m_schema, m_base.keys());
    connect(store, &ConfigStore::published, this, &ConfigDraft::observe);
}
ConfigDraft::~ConfigDraft() {
    ConfigDraftState::get(this)->reset();
    closePreviewScopes();
    // Scoped preview consumers must observe closure before QObject::destroyed
    // reaches their native detached/status forwarding connections.
    ConfigChangePublisher::prepare(this, {{m_previewChanges.get(), paths()}});
}
bool ConfigDraft::prepareChanges(const QList<ConfigPath> &paths) {
    return ConfigChangePublisher::prepare(
        this, {{m_changes.get(), paths}, {m_previewChanges.get(), paths}});
}
QVariant ConfigDraft::previewValue(const ConfigPath &path) const {
    if (!m_store || !m_base.contains(path))
        return {};
    const auto edit = m_edits.constFind(path);
    const auto state = ConfigDraftState::get(this);
    if (state->hasRemovals() && (edit == m_edits.cend() || !edit.value()))
        return state->live(path);
    return edit == m_edits.cend() ? m_store->value(path) : edit.value().value_or(QVariant());
}
QVariant ConfigDraft::previewPresentation(const ConfigPath &path) const {
    if (!m_previewPresentations.contains(path)) {
        const auto descriptor = m_previewSchema.descriptor(path);
        m_previewPresentations[ownPath(path)] =
            descriptor ? collectionPresentation(*descriptor, previewValue(path)) : QVariant();
    }
    return m_previewPresentations.value(path);
}
ConfigStoreHandle *ConfigDraft::previewScope(const ConfigPath &path, QObject *parent) {
    const auto descriptor = m_previewSchema.descriptor(path);
    if (!valid() || !descriptor || descriptor->type != ConfigDescriptor::Type::Group)
        return nullptr;
    if (!m_previewLifetime || !*m_previewLifetime) {
        m_previewId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_previewLifetime = std::make_shared<std::atomic_bool>(true);
        publishPreview();
    }
    return new ConfigStoreHandle(this, path, parent ? parent : this);
}
void ConfigDraft::publishPreview() {
    m_previewPresentations.clear();
    if (!m_store || !m_previewLifetime || !*m_previewLifetime)
        return;
    auto preview = std::make_shared<ConfigDraftPreview>();
    preview->id = m_previewId;
    preview->epoch = ConfigDraftState::get(this)->nextPreviewEpoch();
    preview->lifetime = m_previewLifetime;
    preview->paths = paths();
    preview->edits = m_edits;
    ConfigCommand command;
    command.operation = "draftPreview";
    command.preview = preview;
    m_store->m_owner->dispatch(command);
}
void ConfigDraft::closePreviewScopes() {
    if (!m_previewLifetime || !m_previewLifetime->exchange(false))
        return;
    if (m_store)
        m_store->m_owner->overlayEndpoint()->send(
            [lifetime = m_previewLifetime](ConfigWorker &w) { w.overlayScopeClosed(lifetime); });
}
QVariant ConfigDraft::value(const ConfigPath &path) const {
    const auto edit = m_edits.constFind(path);
    if (edit != m_edits.cend() && !edit.value())
        return ConfigDraftState::get(this)->frozen(path);
    return edit == m_edits.cend() ? m_base.value(path) : edit.value().value_or(QVariant());
}
QVariant ConfigDraft::presentation(const ConfigPath &path) const {
    auto it = m_presentations.constFind(path);
    if (it != m_presentations.cend())
        return it.value();
    const auto descriptor = m_schema.descriptor(path);
    const auto result = descriptor ? collectionPresentation(*descriptor, value(path)) : QVariant();
    m_presentations.insert(ownPath(path), result);
    return result;
}
ConfigOperation ConfigDraft::setValue(const ConfigPath &path, const QVariant &value,
                                      ConfigDescriptor::Format format) {
    const auto checked = m_schema.validate(path, value, format);
    if (!checked.ok())
        return {false, checked.errors};
    if (!valid() || m_busy || !m_base.contains(path))
        return {false,
                {m_schema.diagnostics().make(path, m_busy ? "draft-busy" : "outside-draft")}};
    const auto ownedPath = ownPath(path);
    const ConfigPublicationScope publication(m_store);
    const bool resolve = ConfigDraftState::get(this)->hasRemovals();
    m_edits[ownedPath] = checked.value;
    m_presentations.remove(ownedPath);
    ConfigReadProjectionState::stage(this);
    if (resolve)
        ConfigDraftState::get(this)->resolve(m_edits, false);
    else {
        ConfigDraftState::get(this)->capture(m_edits);
        publishPreview();
    }
    ConfigConsumerGuard boundary(this);
    if (prepareChanges({ownedPath}) &&
        boundary.invoke([&] { emit previewValuesChanged({ownedPath}); }) &&
        boundary.invoke([&] { emit valuesChanged({ownedPath}); }))
        boundary.invoke([&] { emit stateChanged(); });
    return {true, {}};
}
ConfigOperation ConfigDraft::setValue(const ConfigAdmission &value) {
    const auto checked = m_schema.admitted(value);
    const auto path = value.path();
    if (!checked.ok())
        return {false, checked.errors};
    if (!valid() || m_busy || !m_base.contains(path))
        return {false,
                {m_schema.diagnostics().make(path, m_busy ? "draft-busy" : "outside-draft")}};
    const bool resolve = ConfigDraftState::get(this)->hasRemovals();
    m_edits[path] = checked.value;
    const ConfigPublicationScope publication(m_store);
    m_presentations.remove(path);
    ConfigReadProjectionState::stage(this);
    if (resolve)
        ConfigDraftState::get(this)->resolve(m_edits, false);
    else {
        ConfigDraftState::get(this)->capture(m_edits);
        publishPreview();
    }
    ConfigConsumerGuard boundary(this);
    if (prepareChanges({path}) && boundary.invoke([&] { emit previewValuesChanged({path}); }) &&
        boundary.invoke([&] { emit valuesChanged({path}); }))
        boundary.invoke([&] { emit stateChanged(); });
    return {true, {}};
}
void ConfigDraft::refresh() {
    if (!m_store)
        return;
    for (auto it = m_base.begin(); it != m_base.end(); ++it) {
        it.value() = m_store->value(it.key());
        m_fences[it.key()] = m_store->info(it.key());
    }
    m_presentations.clear();
    m_previewPresentations.clear();
    ConfigReadProjectionState::refresh(this);
    ConfigDraftState::get(this)->capture(m_edits);
}
void ConfigDraft::prepareStoreChange() {
    m_previewPresentations.clear();
    if (m_busy || m_conflicted || !m_store)
        return;
    if (m_admitted && !m_admitted->valid) {
        m_conflicted = true;
        m_stateNotificationPending = true;
        return;
    }
    for (auto it = m_fences.cbegin(); it != m_fences.cend(); ++it) {
        const auto state = m_store->info(it.key());
        if (state.intentRevision != it->intentRevision ||
            (!m_admitted && state.contentRevision != it->contentRevision) ||
            state.pathGeneration != it->pathGeneration) {
            m_conflicted = true;
            m_stateNotificationPending = true;
            return;
        }
    }
}
void ConfigDraft::observe() {
    prepareStoreChange();
    const auto selected = paths();
    const auto stateChange = std::exchange(m_stateNotificationPending, false);
    ConfigConsumerGuard boundary(this);
    if (!ConfigChangePublisher::prepare(
            this, {{m_changes.get(), {}}, {m_previewChanges.get(), selected}}) ||
        !boundary.invoke([&] { emit previewValuesChanged(selected); }))
        return;
    if (ConfigDraftState::get(this)->takeValuesChanged() &&
        !boundary.invoke([&] { emit valuesChanged(selected); }))
        return;
    if (stateChange)
        boundary.invoke([&] { emit stateChanged(); });
}
quint64 ConfigDraft::apply(QObject *context, ConfigStore::Callback callback) {
    return applySelection(std::nullopt, context, std::move(callback), false);
}
quint64 ConfigDraft::applySelection(const std::optional<QList<ConfigPath>> &selection,
                                    QObject *context, ConfigStore::Callback callback,
                                    bool retained) {
    if (!valid() || m_busy || !ConfigDraftState::get(this)->settled())
        return 0;
    // A selected Apply admits only its selected edits. An admitted witness is
    // retried only by the same selection, never recaptured by a different one.
    auto edits = m_edits;
    auto fences = m_fences;
    if (selection) {
        for (auto it = edits.begin(); it != edits.end();)
            it = selection->contains(it.key()) ? std::next(it) : edits.erase(it);
        for (auto it = fences.begin(); it != fences.end();)
            it = selection->contains(it.key()) ? std::next(it) : fences.erase(it);
        if (m_admitted && QSet<ConfigPath>(m_admittedEdits.keyBegin(), m_admittedEdits.keyEnd()) !=
                              QSet<ConfigPath>(edits.keyBegin(), edits.keyEnd()))
            return 0;
    }
    const ConfigPublicationScope publication(m_store);
    m_busy = true;
    if (m_applyId.isEmpty())
        m_applyId = "draft:" + QUuid::createUuid().toString(QUuid::Id128);
    auto attempt = std::make_shared<ConfigApplyAttempt>();
    attempt->ownerId = m_applyId;
    attempt->expected = m_admitted;
    attempt->selectedPublication = ConfigDraftState::get(this)->hasRemovals() || selection;
    for (const auto &edit : m_admittedEdits)
        attempt->selectedPublication |= !edit.has_value();
    const QPointer<ConfigDraft> guard(this);
    const QPointer<QObject> target(context ? context : this);
    // A retained caller (a pending reply) still receives the admitted completion
    // after this presentation is gone; the draft's own updates stay guarded.
    QObject *const owner = retained ? static_cast<QObject *>(m_store.data()) : this;
    const auto id = m_store->applyGroup(
        edits, fences, owner,
        ConfigPairedCallback{[guard, target, attempt, edits, selection, retained,
                              callback = std::move(callback)](const ConfigCompletion &completion) {
            const auto &result = completion.result;
            if (!guard) {
                if (retained && target && callback)
                    invokeCompletion(callback, completion);
                return;
            }
            guard->m_busy = false;
            if (result.status != "failed" && selection) {
                guard->m_admitted.reset();
                guard->m_admittedEdits.clear();
                for (auto it = edits.cbegin(); it != edits.cend(); ++it)
                    guard->m_edits.remove(it.key());
                guard->rebase(*selection);
            } else if (result.status != "failed") {
                guard->m_admitted.reset();
                guard->m_admittedEdits.clear();
                guard->closePreviewScopes();
                ConfigDraftState::get(guard)->reset();
                guard->m_edits.clear();
                guard->m_conflicted = false;
                guard->refresh();
            } else {
                if (attempt->admitted) {
                    guard->m_admitted = attempt->admitted;
                    guard->m_admittedEdits = edits;
                    if (selection)
                        for (auto it = attempt->admitted->fences.cbegin();
                             it != attempt->admitted->fences.cend(); ++it)
                            guard->m_fences[it.key()] = it.value();
                    else
                        guard->m_fences = attempt->admitted->fences;
                }
                // Admission rejection covers ingress, but accepts no intent. Reconcile only
                // this attempt's single generation step, never a changed baseline or a later
                // local write. Admitted IO failures instead use their worker-owned witness above.
                for (auto it = result.rejectedPathGenerations.cbegin();
                     guard->m_store && it != result.rejectedPathGenerations.cend(); ++it) {
                    auto fence = guard->m_fences.find(it.key());
                    const auto state = guard->m_store->info(it.key());
                    if (fence != guard->m_fences.end() && fence->pathGeneration + 1 == it.value() &&
                        state.pathGeneration == it.value() &&
                        state.latestInput == result.inputPrefix &&
                        state.workerCovered >= result.inputPrefix &&
                        state.intentRevision == fence->intentRevision &&
                        state.contentRevision == fence->contentRevision)
                        fence->pathGeneration = it.value();
                }
                if (result.error.code == "draft-conflict" || result.error.code == "path-conflict")
                    guard->m_conflicted = true;
                if (!guard->prepareChanges({}))
                    return;
                guard->observe(); // Publications skipped while busy still count as foreign work.
                if (!guard)
                    return;
            }
            ConfigConsumerGuard boundary(guard);
            const auto paths = guard->paths();
            if (!guard->prepareChanges(paths))
                return;
            if ((result.status != "failed" && guard->m_edits.isEmpty() &&
                 !boundary.invoke([&] { emit guard->previewScopesClosed(); })) ||
                !boundary.invoke([&] { emit guard->previewValuesChanged(paths); }) ||
                !boundary.invoke([&] { emit guard->valuesChanged(paths); }) ||
                !boundary.invoke([&] { emit guard->stateChanged(); }))
                return;
            if (!boundary.invoke([&] {
                    ConfigReadProjectionState::operation(guard->readProjection(), completion);
                }))
                return;
            if (target && callback) {
                if (target->thread() == QThread::currentThread())
                    invokeCompletion(callback, completion);
                else
                    QMetaObject::invokeMethod(
                        target,
                        [target, callback, completion] {
                            if (target)
                                invokeCompletion(callback, completion);
                        },
                        Qt::QueuedConnection);
            }
        }},
        attempt, selection ? edits : m_admittedEdits);
    if (prepareChanges({}))
        ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}
void ConfigDraft::rebase(const QList<ConfigPath> &selection) {
    // Explicit rebase of selected leaves only; other staged edits keep their
    // frozen bases and fences. The remaining batch is recaptured as a whole.
    for (const auto &path : selection)
        if (m_base.contains(path) && m_store) {
            m_base[path] = m_store->value(path);
            m_fences[path] = m_store->info(path);
        }
    m_presentations.clear();
    m_previewPresentations.clear();
    auto *state = ConfigDraftState::get(this);
    state->reset();
    ConfigReadProjectionState::refresh(this);
    ConfigReadProjectionState::stage(this);
    m_conflicted = false;
    if (m_edits.isEmpty()) {
        closePreviewScopes();
        state->capture(m_edits);
        return;
    }
    bool removals = false;
    for (const auto &edit : std::as_const(m_edits))
        removals |= !edit.has_value();
    if (removals)
        state->resolve(m_edits, false);
    else {
        state->capture(m_edits);
        publishPreview();
    }
    prepareStoreChange();
}
bool ConfigDraft::discardSelection(const QList<ConfigPath> &selection) {
    if (!valid() || m_busy)
        return false;
    for (auto it = m_admittedEdits.cbegin(); it != m_admittedEdits.cend(); ++it)
        if (selection.contains(it.key()))
            return false; // An admitted Retry witness is never discarded here.
    const ConfigPublicationScope publication(m_store);
    for (const auto &path : selection)
        m_edits.remove(path);
    rebase(selection);
    const auto changed = paths();
    ConfigConsumerGuard boundary(this);
    if (prepareChanges(changed) &&
        (!m_edits.isEmpty() || boundary.invoke([&] { emit previewScopesClosed(); })) &&
        boundary.invoke([&] { emit previewValuesChanged(changed); }) &&
        boundary.invoke([&] { emit valuesChanged(changed); }))
        boundary.invoke([&] { emit stateChanged(); });
    return true;
}
bool ConfigDraft::discard() {
    if (!valid() || m_busy)
        return false;
    const ConfigPublicationScope publication(m_store);
    closePreviewScopes();
    ConfigDraftState::get(this)->reset();
    m_admitted.reset();
    m_admittedEdits.clear();
    m_edits.clear();
    m_conflicted = false;
    refresh();
    const auto changed = paths();
    ConfigConsumerGuard boundary(this);
    if (prepareChanges(changed) && boundary.invoke([&] { emit previewScopesClosed(); }) &&
        boundary.invoke([&] { emit previewValuesChanged(changed); }) &&
        boundary.invoke([&] { emit valuesChanged(changed); }))
        boundary.invoke([&] { emit stateChanged(); });
    return true;
}
quint64 ConfigDraft::abandonApply(QObject *context, ConfigStore::Callback callback) {
    if (!valid() || m_busy)
        return 0;
    const ConfigPublicationScope publication(m_store);
    m_busy = true;
    auto attempt = std::make_shared<ConfigApplyAttempt>();
    attempt->ownerId = m_applyId;
    attempt->expected = m_admitted;
    const QPointer<ConfigDraft> guard(this);
    const QPointer<QObject> target(context ? context : this);
    const auto admitted = m_admittedEdits.keys();
    const auto id = m_store->registerRequest(
        this, ConfigPairedCallback{[guard, target, admitted, callback = std::move(callback)](
                                       const ConfigCompletion &completion) {
            if (!guard)
                return;
            guard->m_busy = false;
            if (completion.result.status == "abandoned") {
                // The witness is gone; its paths rebase onto the restored saved
                // values while the user's staged edits remain staged.
                guard->m_admitted.reset();
                guard->m_admittedEdits.clear();
                guard->m_applyId.clear();
                guard->rebase(admitted);
            }
            ConfigConsumerGuard boundary(guard);
            const auto paths = guard->paths();
            if (!guard->prepareChanges(paths) ||
                !boundary.invoke([&] { emit guard->previewValuesChanged(paths); }) ||
                !boundary.invoke([&] { emit guard->valuesChanged(paths); }) ||
                !boundary.invoke([&] { emit guard->stateChanged(); }))
                return;
            if (!boundary.invoke([&] {
                    ConfigReadProjectionState::operation(guard->readProjection(), completion);
                }))
                return;
            if (target && callback) {
                if (target->thread() == QThread::currentThread())
                    invokeCompletion(callback, completion);
                else
                    QMetaObject::invokeMethod(
                        target,
                        [target, callback, completion] {
                            if (target)
                                invokeCompletion(callback, completion);
                        },
                        Qt::QueuedConnection);
            }
        }});
    ConfigCommand command{id, m_store->m_sequence, "abandonApply", {}};
    command.applyAttempt = attempt;
    if (!m_store->m_accepting || !m_store->m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_store->m_storeId;
        result.operation = "abandonApply";
        result.status = "failed";
        result.inputPrefix = m_store->m_sequence;
        result.error = {"store-closed", {}, "abandonApply"};
        QMetaObject::invokeMethod(
            m_store,
            [store = m_store, result] {
                if (store)
                    store->receiveResult(result);
            },
            Qt::QueuedConnection);
    }
    if (prepareChanges({}))
        ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}
ConfigDraft *ConfigStore::draft(const QList<ConfigPath> &paths, QObject *parent) {
    return new ConfigDraft(this, paths, parent ? parent : this);
}
} // namespace configqml
