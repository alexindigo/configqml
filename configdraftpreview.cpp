// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configdraft.h"
#include "configdraft_p.h"
#include "configshutdown_p.h"
#include "configreadprojection_p.h"
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
             const ConfigCompletion &completion) {
    if (!target || !callback)
        return;
    if (target->thread() == QThread::currentThread())
        invokeCompletion(callback, completion);
    else
        QMetaObject::invokeMethod(
            target,
            [target, callback, completion] {
                ConfigConsumerGuard(target).invoke([&] { invokeCompletion(callback, completion); });
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
    m_frozenRead = m_liveRead = {};
    m_error.clear();
}
void ConfigDraftState::capture(const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    resolveRequest(edits, false, nullptr, {}, {}, true);
}
quint64 ConfigDraftState::resolve(const QMap<ConfigPath, std::optional<QVariant>> &edits,
                                  bool stage, QObject *context, ConfigStore::Callback callback,
                                  const QString &failure) {
    return resolveRequest(edits, stage, context, std::move(callback), failure, false);
}
quint64 ConfigDraftState::resolveRequest(const QMap<ConfigPath, std::optional<QVariant>> &edits,
                                         bool stage, QObject *context,
                                         ConfigStore::Callback callback, const QString &failure,
                                         bool control) {
    auto *store = m_draft->m_store.data();
    const QPointer<QObject> target(context ? context : m_draft);
    const QPointer<ConfigDraft> draft(m_draft);
    const auto id =
        control ? 0
                : store->registerRequest(
                      this, ConfigPairedCallback{[draft, target,
                                                  callback](const ConfigCompletion &completion) {
                          if (!draft)
                              return;
                          if (ConfigReadProjectionState::operation(draft->readProjection(),
                                                                   completion) &&
                              draft)
                              deliver(target, callback, completion);
                      }});
    const auto operation = control ? QString("draftCapture")
                           : stage ? QString("stageReset")
                                   : QString("draftRefresh");
    if (!failure.isEmpty() || !store->m_accepting ||
        store->status() != ConfigStore::Status::Ready) {
        if (control)
            return 0;
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
    request->previewId = m_draft->m_previewId;
    request->previewLifetime = m_draft->m_previewLifetime;
    request->previewEpoch = m_previewEpoch + 1;
    ConfigCommand command{id, store->m_sequence, operation, {}};
    command.draft = request;
    if (!control) {
        m_pending = id;
        m_pendingStage = stage;
        m_previousSettled = m_settled;
        m_settled = false;
        m_draft->m_busy = true;
    }
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
        if (!state->current(*p.request) || p.generation <= state->m_generation)
            continue;
        if (p.requestId && p.requestId != state->m_pending)
            continue;
        state->m_generation = p.generation;
        state->m_error = p.error;
        if (p.error.isEmpty()) {
            state->m_frozen = p.frozen;
            state->m_live = p.live;
            state->m_frozenRead = p.frozenRead;
            state->m_liveRead = p.liveRead;
            if (p.requestId)
                draft->m_edits = p.request->edits;
            state->m_removals = removals(draft->m_edits);
            state->m_acceptedEpoch = state->m_epoch;
            state->m_settled = true;
            state->m_valuesPending |= bool(p.requestId);
        } else if (p.requestId)
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
    const ConfigPublicationScope publication(m_store);
    const auto selected = ConfigSelection::capture(
        m_schema, selection, paths(), ConfigSelection::Target::UserLayer, "draft-selection");
    auto edits = m_edits;
    for (const auto &path : selected.leaves)
        edits[path] = std::nullopt;
    const auto id = ConfigDraftState::get(this)->resolve(edits, true, context, std::move(callback),
                                                         selected.error);
    if (prepareChanges({}))
        ConfigConsumerGuard(this).invoke([&] { emit stateChanged(); });
    return id;
}

QMap<ConfigPath, QVariant>
ConfigWorker::draftCandidate(const ConfigLayers &base, const QList<ConfigPath> &paths,
                             const QMap<ConfigPath, std::optional<QVariant>> &edits,
                             QString &error) const {
    auto selection = ConfigSelection::capture(
        base.schema(), paths, paths, ConfigSelection::Target::UserLayer, "draft-selection");
    selection.owner = m_endpoint;
    const auto candidate = resolveCandidate(base, ConfigEditBatch::capture(selection, edits),
                                            m_listPolicies, m_listMetadata);
    if (!candidate.valid()) {
        error = candidate.error;
        return {};
    }
    // Explicit set previews keep their existing authored-value semantics.
    return candidatePresentation(candidate);
}
ConfigCandidate
ConfigWorker::resolveCandidate(const ConfigLayers &base, const ConfigEditBatch &batch,
                               const QMap<QString, ConfigRecordListPolicySpec> &policies,
                               const QMap<QString, ConfigRecordListMetadata> &metadata,
                               const std::function<QString(const ConfigInput &)> &admission) const {
    ++m_candidateResolutions;
    return configqml::resolveCandidate(base, batch, policies, metadata, admission);
}
QVariantList ConfigWorker::candidateKey(const QList<ConfigPath> &paths) const {
    QVariantList key;
    for (const auto &layer : m_layers->m_layers)
        for (const auto &state : layer.sources) {
            const auto &source = state.source;
            // A complete source is a dependency: an opaque sibling can change
            // whole-source fallback after removal. Projection callbacks can
            // compose sources, so retain their declared context conservatively.
            bool selected = !m_options.projections.isEmpty();
            for (const auto &path : paths)
                selected |= startsWith(path, source.prefix) || startsWith(source.prefix, path);
            if (!selected)
                continue;
            key.append(QVariant(QVariantList{layer.descriptor.id, source.id, source.raw,
                                             source.exists, source.available, source.localIntent,
                                             state.admissionSource.raw, state.rejected,
                                             state.projectionError}));
            if (!m_options.projections.isEmpty())
                key.append(QVariant(QVariantList{source.bytes, source.revision}));
        }
    for (const auto &policy : m_listPolicies)
        if (paths.contains(policy.path))
            key.append(
                QVariant(QVariantList{policy.policyId, m_listRevisions.value(policy.policyId)}));
    return key;
}
void ConfigWorker::resolveDraft(const ConfigCommand &command) {
    const auto request = command.draft;
    auto result = resultFor(command);
    QString error;
    auto previous = m_draftCaptures.value(request->id);
    if (!request->lifetime || !*request->lifetime)
        error = "draft-closed";
    if (command.operation == "stageReset" || !previous.frozen)
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
    const auto frozenObservation =
        previous.frozen ? previous.frozenObservation : readSnapshot(request->paths).observation();
    ConfigDraftPresentation p;
    QMap<ConfigPath, ConfigCandidateFact> liveFacts;
    p.request = request;
    p.requestId = command.requestId;
    if (error.isEmpty()) {
        auto selection =
            ConfigSelection::capture(m_options.schema, request->paths, request->paths,
                                     ConfigSelection::Target::UserLayer, "draft-selection");
        selection.owner = m_endpoint;
        selection.lifetime = request->lifetime;
        const auto batch = ConfigEditBatch::capture(selection, request->edits);
        const auto frozen = resolveCandidate(*base, batch, m_listPolicies, m_listMetadata);
        const auto liveCandidate =
            previous.frozen ? resolveCandidate(*m_layers, batch, m_listPolicies, m_listMetadata)
                            : ConfigCandidate{};
        const auto &live = previous.frozen ? liveCandidate : frozen;
        error = !frozen.valid() ? frozen.error : live.error;
        if (error.isEmpty()) {
            p.frozen = candidatePresentation(frozen);
            p.live = candidatePresentation(live);
            liveFacts = live.facts;
            const auto observation = [&](const ConfigObservation &base, bool preview) {
                auto dependencies = ConfigObservationAccess::dependencies(base);
                dependencies.first().previewId =
                    preview && !request->previewId.isEmpty() ? request->previewId : request->id;
                dependencies.first().previewLifetime = preview && request->previewLifetime
                                                           ? request->previewLifetime
                                                           : request->lifetime;
                dependencies.first().previewEpoch =
                    preview && request->previewLifetime ? request->previewEpoch : request->epoch;
                return ConfigObservationAccess::observation(dependencies);
            };
            p.frozenRead = ConfigObservationAccess::candidate(
                frozen, m_options.schema, observation(frozenObservation, false));
            p.liveRead = ConfigObservationAccess::candidate(
                live, m_options.schema,
                observation(readSnapshot(request->paths).observation(), true));
        }
    }
    p.error = error;
    result.status = error.isEmpty() ? "staged" : "failed";
    if (!error.isEmpty())
        result.error = {error, {}, command.operation};
    else
        m_draftCaptures[request->id] = {
            request,           base,         p.frozen,  p.live,
            frozenObservation, p.frozenRead, liveFacts, candidateKey(request->paths)};
    m_draftPackets.append(p);
    auto packet = publish(command.requestId ? std::optional<ConfigResult>(result) : std::nullopt);
    if (error.isEmpty() && command.requestId)
        packet.completionObservation = p.frozenRead.observation();
    post(packet);
}
QList<ConfigDraftPresentation> ConfigWorker::draftPresentations() {
    auto packets = std::exchange(m_draftPackets, {});
    for (auto it = m_draftCaptures.begin(); it != m_draftCaptures.end();) {
        if (!*it->request->lifetime) {
            it = m_draftCaptures.erase(it);
            continue;
        }
        bool captured = false;
        for (const auto &packet : packets)
            captured |= packet.request == it->request;
        if (captured) {
            ++it;
            continue;
        }
        const auto key = candidateKey(it->request->paths);
        if (key == it->liveKey) {
            ++it;
            continue; // A publication/control cursor is not a candidate invalidation.
        }
        it->liveKey = key;
        auto selection =
            ConfigSelection::capture(m_options.schema, it->request->paths, it->request->paths,
                                     ConfigSelection::Target::UserLayer, "draft-selection");
        const auto candidate =
            resolveCandidate(*m_layers, ConfigEditBatch::capture(selection, it->request->edits),
                             m_listPolicies, m_listMetadata);
        const auto error = candidate.error;
        const auto live = candidatePresentation(candidate);
        if (!error.isEmpty() || candidate.facts != it->liveFacts) {
            it->liveValues = live;
            it->liveFacts = candidate.facts;
            ConfigDraftPresentation presentation;
            presentation.request = it->request;
            presentation.error = error;
            presentation.frozen = it->frozenValues;
            presentation.live = live;
            presentation.frozenRead = it->frozenRead;
            auto dependencies = ConfigObservationAccess::dependencies(
                readSnapshot(it->request->paths).observation());
            auto &dependency = dependencies.first();
            dependency.previewId =
                it->request->previewId.isEmpty() ? it->request->id : it->request->previewId;
            dependency.previewLifetime =
                it->request->previewLifetime ? it->request->previewLifetime : it->request->lifetime;
            dependency.previewEpoch =
                it->request->previewLifetime ? it->request->previewEpoch : it->request->epoch;
            presentation.liveRead = ConfigObservationAccess::candidate(
                candidate, m_options.schema, ConfigObservationAccess::observation(dependencies));
            packets.append(presentation);
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
