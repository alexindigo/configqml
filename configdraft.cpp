// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configconsumer_p.h"
#include "configprojection_p.h"
#include "configdraft_p.h"
#include "configapply_p.h"
#include "configstorehandle.h"
#include "configshutdown_p.h"
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
ConfigDraft::~ConfigDraft() { closePreviewScopes(); }
QVariant ConfigDraft::previewValue(const ConfigPath &path) const {
    if (!m_store || !m_base.contains(path))
        return {};
    const auto edit = m_edits.constFind(path);
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
    m_edits[ownedPath] = checked.value;
    m_presentations.remove(ownedPath);
    publishPreview();
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit previewValuesChanged({ownedPath}); }) &&
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
    m_edits[path] = checked.value;
    m_presentations.remove(path);
    publishPreview();
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit previewValuesChanged({path}); }) &&
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
}
void ConfigDraft::observe() {
    m_previewPresentations.clear();
    if (!ConfigConsumerGuard(this).invoke([&] { emit previewValuesChanged(paths()); }))
        return;
    if (m_busy || m_conflicted || !m_store)
        return;
    if (m_admitted && !m_admitted->valid) {
        m_conflicted = true;
        ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
        return;
    }
    for (auto it = m_fences.cbegin(); it != m_fences.cend(); ++it) {
        const auto state = m_store->info(it.key());
        if (state.intentRevision != it->intentRevision ||
            (!m_admitted && state.contentRevision != it->contentRevision) ||
            state.pathGeneration != it->pathGeneration) {
            m_conflicted = true;
            ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
            return;
        }
    }
}
quint64 ConfigDraft::apply(QObject *context, ConfigStore::Callback callback) {
    if (!valid() || m_busy)
        return 0;
    m_busy = true;
    if (m_applyId.isEmpty())
        m_applyId = "draft:" + QUuid::createUuid().toString(QUuid::Id128);
    auto attempt = std::make_shared<ConfigApplyAttempt>();
    attempt->ownerId = m_applyId;
    attempt->expected = m_admitted;
    const auto edits = m_edits;
    const QPointer<ConfigDraft> guard(this);
    const QPointer<QObject> target(context ? context : this);
    const auto id = m_store->applyGroup(
        m_edits, m_fences, this,
        [guard, target, attempt, edits,
         callback = std::move(callback)](const ConfigResult &result) {
            if (!guard)
                return;
            guard->m_busy = false;
            if (result.status != "failed") {
                guard->m_admitted.reset();
                guard->m_admittedEdits.clear();
                guard->closePreviewScopes();
                guard->m_edits.clear();
                guard->m_conflicted = false;
                guard->refresh();
            } else {
                if (attempt->admitted) {
                    guard->m_admitted = attempt->admitted;
                    guard->m_admittedEdits = edits;
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
                guard->observe(); // Publications skipped while busy still count as foreign work.
                if (!guard)
                    return;
            }
            ConfigConsumerGuard boundary(guard);
            const auto paths = guard->paths();
            if ((result.status != "failed" &&
                 !boundary.invoke([&] { emit guard->previewScopesClosed(); })) ||
                !boundary.invoke([&] { emit guard->previewValuesChanged(paths); }) ||
                !boundary.invoke([&] { emit guard->valuesChanged(paths); }) ||
                !boundary.invoke([&] { emit guard->stateChanged(); }))
                return;
            if (target && callback) {
                if (target->thread() == QThread::currentThread())
                    callback(result);
                else
                    QMetaObject::invokeMethod(
                        target,
                        [target, callback, result] {
                            if (target)
                                callback(result);
                        },
                        Qt::QueuedConnection);
            }
        },
        attempt, m_admittedEdits);
    ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}
bool ConfigDraft::discard() {
    if (!valid() || m_busy)
        return false;
    closePreviewScopes();
    m_admitted.reset();
    m_admittedEdits.clear();
    m_edits.clear();
    m_conflicted = false;
    refresh();
    const auto changed = paths();
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit previewScopesClosed(); }) &&
        boundary.invoke([&] { emit previewValuesChanged(changed); }) &&
        boundary.invoke([&] { emit valuesChanged(changed); }))
        boundary.invoke([&] { emit stateChanged(); });
    return true;
}
ConfigDraft *ConfigStore::draft(const QList<ConfigPath> &paths, QObject *parent) {
    return new ConfigDraft(this, paths, parent ? parent : this);
}
} // namespace configqml
