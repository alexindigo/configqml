// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configconsumer_p.h"
#include "configprojection_p.h"

namespace configqml {
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
    connect(store, &ConfigStore::published, this, &ConfigDraft::observe);
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
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged({ownedPath}); }))
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
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged({path}); }))
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
}
void ConfigDraft::observe() {
    if (m_busy || m_conflicted || !m_store)
        return;
    for (auto it = m_fences.cbegin(); it != m_fences.cend(); ++it) {
        const auto state = m_store->info(it.key());
        if (state.intentRevision != it->intentRevision ||
            state.contentRevision != it->contentRevision ||
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
    const QPointer<ConfigDraft> guard(this);
    const QPointer<QObject> target(context ? context : this);
    const auto id = m_store->applyGroup(
        m_edits, m_fences, this,
        [guard, target, callback = std::move(callback)](const ConfigResult &result) {
            if (!guard)
                return;
            guard->m_busy = false;
            if (result.status != "failed") {
                guard->m_edits.clear();
                guard->m_conflicted = false;
                guard->refresh();
            } else {
                // Admission rejection covers ingress, but accepts no intent. Reconcile only
                // this attempt's single generation step, never a changed baseline or a later
                // local write. Persistence failures deliberately carry no such settlement.
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
            }
            ConfigConsumerGuard boundary(guard);
            const auto paths = guard->paths();
            if (!boundary.invoke([&] { emit guard->valuesChanged(paths); }) ||
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
        });
    ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}
bool ConfigDraft::discard() {
    if (!valid() || m_busy)
        return false;
    m_edits.clear();
    m_conflicted = false;
    refresh();
    const auto changed = paths();
    ConfigConsumerGuard boundary(this);
    if (boundary.invoke([&] { emit valuesChanged(changed); }))
        boundary.invoke([&] { emit stateChanged(); });
    return true;
}
ConfigDraft *ConfigStore::draft(const QList<ConfigPath> &paths, QObject *parent) {
    return new ConfigDraft(this, paths, parent ? parent : this);
}
} // namespace configqml
