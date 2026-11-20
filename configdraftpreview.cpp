// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configdraft_p.h"
#include "configshutdown_p.h"
#include <QUuid>

namespace configqml {
namespace {
constexpr auto stateName = "_configqml_draft_state";
constexpr auto registryName = "_configqml_draft_registry";
class DraftRegistry : public QObject {
public:
    using QObject::QObject;
    QMap<QString, QPointer<QObject>> entries;
};
DraftRegistry *registry(ConfigStore *store) {
    const auto existing = store->findChild<QObject *>(registryName, Qt::FindDirectChildrenOnly);
    if (existing)
        return static_cast<DraftRegistry *>(existing);
    auto *result = new DraftRegistry(store);
    result->setObjectName(registryName);
    return result;
}
void deliver(const QPointer<QObject> &target, const ConfigStore::Callback &callback,
             const ConfigResult &result) {
    if (!target || !callback)
        return;
    if (target->thread() == QThread::currentThread())
        callback(result);
    else
        QMetaObject::invokeMethod(
            target,
            [target, callback, result] {
                ConfigConsumerGuard(target).invoke([&] { callback(result); });
            },
            Qt::QueuedConnection);
}
bool removals(const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    for (const auto &edit : edits)
        if (!edit)
            return true;
    return false;
}
} // namespace

ConfigDraftState::ConfigDraftState(ConfigDraft *draft)
    : QObject(draft), m_draft(draft), m_id(QUuid::createUuid().toString(QUuid::Id128)),
      m_lifetime(std::make_shared<std::atomic_bool>(true)) {
    setObjectName(stateName);
    auto *delivery = registry(draft->m_store);
    delivery->entries.removeIf([](const auto &entry) { return !entry.value(); });
    delivery->entries[m_id] = this;
}
ConfigDraftState::~ConfigDraftState() { m_lifetime->store(false); }
ConfigDraftState *ConfigDraftState::get(const ConfigDraft *draft) {
    return static_cast<ConfigDraftState *>(
        draft->findChild<QObject *>(stateName, Qt::FindDirectChildrenOnly));
}
bool ConfigDraftState::hasRemovals() const { return m_removals; }
QString ConfigDraft::error() const {
    return m_error.isEmpty() ? ConfigDraftState::get(this)->error() : m_error;
}
void ConfigDraftState::reset() {
    const auto old = m_lifetime;
    old->store(false);
    if (m_draft->m_store)
        m_draft->m_store->m_owner->overlayEndpoint()->send(
            [old](ConfigWorker &worker) { worker.releaseDraft(old); });
    m_lifetime = std::make_shared<std::atomic_bool>(true);
    ++m_epoch;
    m_acceptedEpoch = m_epoch;
    m_generation = m_pending = 0;
    m_settled = true;
    m_removals = false;
    m_valuesPending = false;
    m_frozen.clear();
    m_live.clear();
    m_error.clear();
}
quint64 ConfigDraftState::resolve(const QMap<ConfigPath, std::optional<QVariant>> &edits,
                                  bool stage, QObject *context, ConfigStore::Callback callback,
                                  const QString &failure) {
    auto *store = m_draft->m_store.data();
    const QPointer<QObject> target(context ? context : m_draft);
    const QPointer<ConfigDraft> draft(m_draft);
    const auto id = store->registerRequest(this, [draft, target, callback](const ConfigResult &r) {
        if (draft)
            deliver(target, callback, r);
    });
    const auto operation = stage ? QString("stageReset") : QString("draftRefresh");
    if (!failure.isEmpty() || !store->m_accepting ||
        store->status() != ConfigStore::Status::Ready) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = store->m_storeId;
        result.operation = operation;
        result.status = "failed";
        result.error = {failure.isEmpty() ? QString("store-closed") : failure, {}, operation};
        QMetaObject::invokeMethod(
            store, [store, result] { store->receiveResult(result); }, Qt::QueuedConnection);
        return id;
    }
    auto request = std::make_shared<ConfigDraftRequest>();
    request->id = m_id;
    request->lifetime = m_lifetime;
    request->epoch = ++m_epoch;
    request->paths = m_draft->paths();
    request->fences = m_draft->m_fences;
    for (const auto &path : request->paths)
        request->ingress[path] = store->m_owner->overlayEndpoint()->revision(path);
    request->edits = edits;
    ConfigCommand command{id, store->m_sequence, operation, {}};
    command.draft = request;
    m_pending = id;
    m_pendingStage = stage;
    m_previousSettled = m_settled;
    m_settled = false;
    m_draft->m_busy = true;
    if (!store->m_owner->dispatch(command)) {
        ConfigWorkerPacket packet;
        ConfigResult result;
        result.requestId = id;
        result.storeId = store->m_storeId;
        result.operation = operation;
        result.status = "failed";
        result.error = {"store-closed", {}, operation};
        packet.result = result;
        QMetaObject::invokeMethod(
            store, [store, packet] { store->receive(packet); }, Qt::QueuedConnection);
    }
    return id;
}
void ConfigDraftState::install(ConfigStore *store, const ConfigWorkerPacket &packet,
                               QList<ConfigChangePublisher::Change> &preparations) {
    const auto existing = store->findChild<QObject *>(registryName, Qt::FindDirectChildrenOnly);
    if (!existing)
        return;
    auto *entries = static_cast<DraftRegistry *>(existing);
    for (const auto &p : packet.drafts) {
        const auto object = entries->entries.value(p.request->id);
        if (!object)
            continue;
        auto *state = static_cast<ConfigDraftState *>(object.data());
        auto *draft = state->m_draft;
        if (p.request->lifetime != state->m_lifetime || !*state->m_lifetime ||
            p.request->epoch != state->m_epoch || p.generation <= state->m_generation)
            continue;
        if (p.requestId && p.requestId != state->m_pending)
            continue;
        state->m_generation = p.generation;
        state->m_error = p.error;
        if (p.error.isEmpty()) {
            state->m_frozen = p.frozen;
            state->m_live = p.live;
            if (p.requestId)
                draft->m_edits = p.request->edits;
            state->m_removals = removals(draft->m_edits);
            state->m_acceptedEpoch = state->m_epoch;
            state->m_settled = true;
            state->m_valuesPending |= bool(p.requestId);
        } else
            state->m_settled =
                p.requestId && state->m_pendingStage ? state->m_previousSettled : false;
        if (p.requestId) {
            if (!p.error.isEmpty() && state->m_pendingStage)
                state->m_epoch = state->m_acceptedEpoch;
            state->m_pending = 0;
            draft->m_busy = false;
        }
        draft->m_presentations.clear();
        draft->m_previewPresentations.clear();
        // Candidate values enter every draft namespace before public store handlers.
        preparations.append({draft->m_changes.get(), draft->paths()});
        preparations.append({draft->m_previewChanges.get(), draft->paths()});
        draft->m_stateNotificationPending = true;
        if (p.requestId && p.error.isEmpty())
            draft->publishPreview();
    }
    // Terminal refusals still settle busy before completed(result), without
    // installing any proposed edits or replacing the last checked presentation.
    if (packet.result)
        for (const auto &entry : entries->entries) {
            if (!entry)
                continue;
            auto *state = static_cast<ConfigDraftState *>(entry.data());
            if (state->m_pending != packet.result->requestId || !state->m_pending)
                continue;
            state->m_pending = 0;
            state->m_draft->m_busy = false;
            state->m_error = packet.result->error.code;
            if (state->m_pendingStage)
                state->m_settled = state->m_previousSettled;
            if (state->m_pendingStage)
                state->m_epoch = state->m_acceptedEpoch;
            state->m_draft->m_stateNotificationPending = true;
            preparations.append(ConfigChangePublisher::Change{state->m_draft->m_changes.get(), {}});
        }
}

quint64 ConfigDraft::stageReset(const QList<ConfigPath> &selection, QObject *context,
                                ConfigStore::Callback callback) {
    if (!valid() || m_busy)
        return 0;
    QString error;
    QSet<ConfigPath> seen, selected;
    if (selection.isEmpty())
        error = "draft-selection";
    for (const auto &path : selection) {
        const auto descriptor = m_schema.descriptor(path);
        if (!descriptor || seen.contains(path)) {
            error = "draft-selection";
            break;
        }
        seen.insert(ownPath(path));
        bool found = false;
        for (const auto &leaf : paths())
            if (startsWith(leaf, path)) {
                selected.insert(leaf);
                found = true;
            }
        if (!found)
            error = "draft-selection";
    }
    auto edits = m_edits;
    for (const auto &path : selected)
        edits[path] = std::nullopt;
    const auto id =
        ConfigDraftState::get(this)->resolve(edits, true, context, std::move(callback), error);
    if (prepareChanges({}))
        ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}

QMap<ConfigPath, QVariant>
ConfigWorker::draftCandidate(const ConfigLayers &base, const QList<ConfigPath> &paths,
                             const QMap<ConfigPath, std::optional<QVariant>> &edits,
                             QString &error) const {
    auto candidate = base;
    const auto outcome = candidate.applyEdits(edits);
    if (!outcome.accepted) {
        error =
            outcome.errors.isEmpty() ? QString("draft-resolution") : outcome.errors.first().code;
        return {};
    }
    QMap<ConfigPath, QVariant> values;
    for (const auto &path : paths) {
        const auto edit = edits.constFind(path);
        // Explicit set previews keep their existing authored-value semantics.
        values[path] = edit != edits.cend() && edit.value()
                           ? *edit.value()
                           : candidate.value(path).value_or(QVariant());
    }
    return values;
}
void ConfigWorker::resolveDraft(const ConfigCommand &command) {
    const auto request = command.draft;
    auto result = resultFor(command);
    QString error;
    auto previous = m_draftCaptures.value(request->id);
    if (!request->lifetime || !*request->lifetime)
        error = "draft-closed";
    if (command.operation == "stageReset")
        for (auto it = request->fences.cbegin(); it != request->fences.cend(); ++it)
            if (m_pathIntents.value(it.key()) != it->intentRevision ||
                m_contentRevisions.value(it.key()) != it->contentRevision ||
                m_pathGenerations.value(it.key()) != it->pathGeneration ||
                m_endpoint->revision(it.key()) != request->ingress.value(it.key()))
                error = "draft-conflict";
    if (previous.request && previous.request->lifetime != request->lifetime)
        previous = {};
    if (previous.request && previous.request->epoch >= request->epoch)
        error = "draft-stale";
    const auto base = previous.frozen ? previous.frozen : std::make_shared<ConfigLayers>(*m_layers);
    ConfigDraftPresentation p;
    p.request = request;
    p.requestId = command.requestId;
    if (error.isEmpty()) {
        p.frozen = draftCandidate(*base, request->paths, request->edits, error);
        p.live = draftCandidate(*m_layers, request->paths, request->edits, error);
    }
    p.error = error;
    result.status = error.isEmpty() ? "staged" : "failed";
    if (!error.isEmpty())
        result.error = {error, {}, command.operation};
    else if (removals(request->edits))
        m_draftCaptures[request->id] = {request, base, p.frozen, p.live};
    else
        m_draftCaptures.remove(request->id);
    m_draftPackets.append(p);
    post(publish(result));
}
QList<ConfigDraftPresentation> ConfigWorker::draftPresentations() {
    auto packets = std::exchange(m_draftPackets, {});
    for (auto it = m_draftCaptures.begin(); it != m_draftCaptures.end();) {
        if (!*it->request->lifetime) {
            it = m_draftCaptures.erase(it);
            continue;
        }
        QString error;
        const auto live = draftCandidate(*m_layers, it->request->paths, it->request->edits, error);
        if (!error.isEmpty() || live != it->liveValues) {
            it->liveValues = live;
            packets.append({it->request, 0, 0, error, it->frozenValues, live});
        }
        ++it;
    }
    for (auto &p : packets)
        p.generation = m_generation;
    return packets;
}
void ConfigWorker::releaseDraft(const std::shared_ptr<std::atomic_bool> &lifetime) {
    for (auto it = m_draftCaptures.begin(); it != m_draftCaptures.end();)
        if (it->request->lifetime == lifetime)
            it = m_draftCaptures.erase(it);
        else
            ++it;
}
} // namespace configqml
