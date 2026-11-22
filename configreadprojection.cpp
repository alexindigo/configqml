// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configreadprojection_p.h"
#include "configworker_p.h"
#include "configshutdown_p.h"
#include "configstorehandle.h"
#include <QUuid>

namespace configqml {
ConfigPublicationScope::ConfigPublicationScope(ConfigStore *store) : m_store(store) {
    if (store)
        m_previous = std::exchange(store->m_publishingModels, true);
}
ConfigPublicationScope::~ConfigPublicationScope() {
    if (m_store) {
        m_store->m_publishingModels = m_previous;
        if (!m_previous)
            ConfigReadProjectionState::deliver(m_store);
    }
}
ConfigReadProjection::ConfigReadProjection(QObject *parent)
    : QObject(parent), d(new ConfigReadProjectionState(this)) {}
ConfigReadProjection::~ConfigReadProjection() { d->lifetime->store(false); }
ConfigReadProjectionState::ConfigReadProjectionState(ConfigReadProjection *projection)
    : q(projection), id(QUuid::createUuid().toString(QUuid::Id128)),
      lifetime(std::make_shared<std::atomic_bool>(true)) {}
ConfigReadProjection *ConfigReadProjectionState::create(QObject *owner, ConfigStore *store,
                                                        const QString &name) {
    if (const auto existing =
            owner->findChild<ConfigReadProjection *>(name, Qt::FindDirectChildrenOnly))
        return existing;
    auto *projection = new ConfigReadProjection(owner);
    projection->setObjectName(name);
    projection->d->store = store;
    if (store && owner != store) {
        get(store)->d->views.removeIf([](const auto &entry) { return !entry; });
        get(store)->d->views.append(projection);
    }
    if (store)
        QObject::connect(store, &ConfigStore::statusChanged, projection, [projection] {
            if (!projection->d->live())
                projection->d->invalidate();
        });
    if (store)
        QObject::connect(store, &QObject::destroyed, projection, [projection] {
            QMetaObject::invokeMethod(
                projection, [projection] { projection->d->invalidate(); }, Qt::QueuedConnection);
        });
    return projection;
}
ConfigReadProjection *ConfigReadProjectionState::get(ConfigStore *store) {
    auto *projection = create(store, store, "_configqml_read_projection");
    projection->d->paths = store->schema().leaves();
    return projection;
}
ConfigReadProjection *ConfigReadProjectionState::get(ConfigStoreHandle *handle) {
    auto *projection = create(handle, handle->m_store, "_configqml_read_projection");
    auto &state = *projection->d;
    if (!state.handle)
        QObject::connect(handle, &ConfigStoreHandle::detached, projection,
                         [projection] { projection->d->invalidate(); });
    state.handle = handle;
    state.prefix = handle->m_prefix;
    state.draft = handle->m_previewDraft;
    state.preview = bool(handle->m_previewDraft);
    if (!state.store)
        return projection;
    const auto schema = state.preview ? state.draft->previewSchema() : state.store->schema();
    for (const auto &path : schema.leaves())
        if (startsWith(path, state.prefix) && !state.paths.contains(path))
            state.paths.append(path);
    if (!state.installed.valid() && state.store && !state.preview)
        state.installed = state.completed = ConfigObservationAccess::select(
            get(state.store)->d->completed, state.paths, state.prefix);
    return projection;
}
ConfigReadProjection *ConfigReadProjectionState::get(ConfigOverlayEditor *overlay) {
    auto *projection = create(overlay, overlay->m_store, "_configqml_read_projection");
    if (!projection->d->overlay)
        QObject::connect(overlay, &ConfigOverlayEditor::stateChanged, projection, [projection] {
            if (!projection->d->live())
                projection->d->invalidate();
        });
    projection->d->overlay = overlay;
    projection->d->paths = overlay->schema().leaves();
    projection->d->frozen = overlay->m_mode == ConfigOverlayMode::Editor;
    return projection;
}
ConfigReadProjection *ConfigReadProjectionState::get(ConfigDraft *draft, bool preview) {
    auto *projection =
        create(draft, draft->m_store,
               preview ? "_configqml_preview_projection" : "_configqml_read_projection");
    if (!projection->d->draft)
        QObject::connect(draft, &ConfigDraft::previewScopesClosed, projection,
                         [projection] { projection->d->invalidate(); });
    projection->d->draft = draft;
    projection->d->preview = preview;
    projection->d->frozen = !preview;
    projection->d->paths = draft->paths();
    if (preview && (!draft->m_previewLifetime || !*draft->m_previewLifetime)) {
        draft->m_previewId = QUuid::createUuid().toString(QUuid::Id128);
        draft->m_previewLifetime = std::make_shared<std::atomic_bool>(true);
        draft->publishPreview();
    }
    if (!projection->d->installed.valid() && draft->m_store) {
        const auto captured = ConfigDraftState::get(draft)->read(preview);
        projection->d->installed = projection->d->completed =
            captured.valid() ? captured
                             : ConfigObservationAccess::select(get(draft->m_store)->d->completed,
                                                               draft->paths());
    }
    if (ConfigDraftState::get(draft)->resolving())
        projection->d->installed =
            ConfigObservationAccess::pending(projection->d->installed, draft->m_edits);
    return projection;
}
void ConfigReadProjectionState::refresh(ConfigDraft *draft) {
    if (!draft->m_store)
        return;
    for (const auto &name : {"_configqml_read_projection", "_configqml_preview_projection"})
        if (auto *projection =
                draft->findChild<ConfigReadProjection *>(name, Qt::FindDirectChildrenOnly)) {
            auto &state = *projection->d;
            state.installed =
                ConfigObservationAccess::select(get(draft->m_store)->d->completed, draft->paths());
            state.completed = state.installed;
            state.changed = false;
        }
}
void ConfigReadProjectionState::initialize(ConfigOverlayEditor *overlay,
                                           const ConfigReadCapture &capture) {
    auto *projection = get(overlay);
    projection->d->receive(capture);
    projection->d->completed = projection->d->installed;
    projection->d->changed = false;
}
ConfigOverlayPresentation
ConfigReadProjectionState::eligible(ConfigOverlayEditor *overlay,
                                    const ConfigOverlayPresentation &presentation) {
    auto result = presentation;
    auto &state = *get(overlay)->d;
    if (result.read.snapshot.valid() && result.read.order < state.order)
        result.refreshed = false;
    if (result.deliveryGeneration && result.deliveryGeneration < state.metadataOrder) {
        result.placementToken.clear();
        result.dirtyKnown = false;
    } else
        state.metadataOrder = qMax(state.metadataOrder, result.deliveryGeneration);
    return result;
}
bool ConfigReadProjectionState::live() const {
    return store && store->status() == ConfigStore::Status::Ready &&
           (!handle || handle->attached()) && (!overlay || overlay->ready()) &&
           (!draft || draft->valid()) && *lifetime;
}
bool ConfigReadProjectionState::available(ConfigReadProjection *projection) {
    return projection && projection->d->live();
}
void ConfigReadProjectionState::stage(ConfigOverlayEditor *overlay) {
    if (!overlay->m_store || overlay->m_mode != ConfigOverlayMode::Editor)
        return;
    auto *projection = get(overlay);
    auto &state = *projection->d;
    state.installed = ConfigObservationAccess::pending(state.installed, overlay->m_edits);
    ++state.editEpoch;
    auto request = std::make_shared<ConfigReadRequest>();
    request->projectionId = state.id;
    request->overlayId = overlay->editorId();
    request->editEpoch = state.editEpoch;
    request->selection.lifetime = state.lifetime;
    ConfigCommand command;
    command.operation = "overlayCandidate";
    command.editorId = overlay->editorId();
    command.overlayEdits = overlay->m_edits;
    command.read = request;
    overlay->m_store->m_owner->dispatch(command);
}
void ConfigReadProjectionState::stage(ConfigDraft *draft) {
    for (const auto &name : {"_configqml_read_projection", "_configqml_preview_projection"})
        if (auto *projection =
                draft->findChild<ConfigReadProjection *>(name, Qt::FindDirectChildrenOnly)) {
            auto &state = *projection->d;
            state.installed = ConfigObservationAccess::pending(state.installed, draft->m_edits);
        }
}
QList<ConfigPath> ConfigReadProjectionState::pending(quint64 covered) const {
    QList<ConfigPath> result;
    if (overlay) {
        for (auto it = overlay->m_editSequences.cbegin(); it != overlay->m_editSequences.cend();
             ++it)
            if (it.value() > covered)
                result.append(it.key());
    } else if (store)
        for (const auto &path : paths)
            if (store->info(path).latestInput > covered)
                result.append(path.mid(prefix.size()));
    return result;
}
ConfigSelectedState ConfigReadProjection::selectedState(const ConfigPath &path) const {
    return d->installed.selectedState(path);
}
ConfigReadSnapshot ConfigReadProjection::snapshot() const { return d->installed; }
ConfigObservation ConfigReadProjection::observation() const { return d->completed.observation(); }
quint64 ConfigReadProjection::synchronize(const ConfigObservation &after, QObject *context,
                                          ReadCallback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->preview && d->draft && !d->handle)
        ConfigReadProjectionState::get(d->draft, true);
    const auto id = ++d->next;
    d->requests.insert(id, {after, context ? context : this, std::move(callback), false, {}});
    QString error;
    if (!d->live())
        error = "read-owner";
    else if (d->frozen)
        error = "read-frozen";
    else if (!ConfigObservationAccess::applicable(d->installed.observation(), after))
        error = "read-observation";
    if (!error.isEmpty()) {
        QMetaObject::invokeMethod(
            this, [this, id, error] { d->fail(id, error); }, Qt::QueuedConnection);
        return id;
    }
    auto request = std::make_shared<ConfigReadRequest>();
    request->projectionId = d->id;
    request->requestId = id;
    request->after = after;
    request->prefix = d->prefix;
    request->selection =
        ConfigSelection::capture(d->overlay ? d->overlay->schema() : d->store->schema(), d->paths,
                                 d->paths, ConfigSelection::Target::ReadOnly, "read-selection");
    request->selection.owner = d->store->m_owner->overlayEndpoint();
    request->selection.lifetime = d->lifetime;
    if (d->overlay)
        request->overlayId = d->overlay->editorId();
    if (d->preview && d->draft) {
        request->previewId = d->draft->m_previewId;
        request->previewLifetime = d->draft->m_previewLifetime;
    }
    ConfigCommand command;
    command.operation = d->overlay ? "overlaySynchronize" : "synchronize";
    command.prefix = d->store->m_sequence;
    command.editorId = request->overlayId;
    command.read = request;
    if (!d->store->m_owner->dispatch(command))
        QMetaObject::invokeMethod(
            this, [this, id] { d->fail(id, "read-owner"); }, Qt::QueuedConnection);
    return id;
}
void ConfigReadProjectionState::receive(const ConfigReadCapture &capture) {
    if (capture.editEpoch && capture.editEpoch != editEpoch)
        return;
    const auto error = !capture.error.isEmpty() ? capture.error
                       : capture.snapshot.valid() && !capture.snapshot.observation().valid()
                           ? QString("read-owner")
                           : QString();
    if (capture.requestId && requests.contains(capture.requestId)) {
        requests[capture.requestId].captured = true;
        requests[capture.requestId].error = error;
    }
    if (!*lifetime || (handle && !handle->attached()) ||
        (overlay && (overlay->status() == ConfigOverlayEditor::Status::Error ||
                     overlay->status() == ConfigOverlayEditor::Status::Closed)) ||
        (store && store->status() != ConfigStore::Status::Ready)) {
        if (capture.requestId && requests.contains(capture.requestId))
            requests[capture.requestId].error = "read-owner";
        return;
    }
    if (!error.isEmpty() || capture.order <= order || !capture.snapshot.valid())
        return;
    installed = capture.snapshot;
    order = capture.order;
    coveredSequence = capture.coveredSequence;
    changed = true;
}
void ConfigReadProjectionState::install(ConfigStore *store, const ConfigWorkerPacket &packet) {
    auto *root = get(store);
    QList<QPointer<ConfigReadProjection>> projections{root};
    projections.append(root->d->views);
    for (const auto &projection : projections) {
        if (!projection)
            continue;
        auto &state = *projection->d;
        if (!state.overlay && !state.draft && packet.readSnapshot.valid()) {
            auto capture = ConfigReadCapture{};
            capture.order = packet.deliveryGeneration;
            capture.coveredSequence = packet.snapshot.inputPrefix;
            capture.snapshot =
                ConfigObservationAccess::select(packet.readSnapshot, state.paths, state.prefix);
            state.receive(capture);
        }
        for (const auto &capture : packet.reads)
            if (capture.projectionId == state.id)
                state.receive(capture);
        if (state.overlay)
            for (const auto &presentation : packet.overlays)
                if (presentation.editorId == state.overlay->editorId() &&
                    presentation.read.snapshot.valid())
                    state.receive(presentation.read);
        if (state.draft)
            for (const auto &presentation : packet.drafts)
                if (presentation.request->id == ConfigDraftState::get(state.draft)->id()) {
                    if (!ConfigDraftState::get(state.draft)->current(*presentation.request))
                        continue;
                    ConfigReadCapture capture;
                    capture.order = presentation.generation;
                    capture.coveredSequence = packet.snapshot.inputPrefix;
                    capture.snapshot =
                        state.preview ? presentation.liveRead : presentation.frozenRead;
                    state.receive(capture);
                }
    }
}
bool ConfigReadProjectionState::prepare() {
    ConfigConsumerGuard guard(q);
    if (changed) {
        changed = false;
        completed = installed;
        if (!guard.invoke([&] { emit q->stateChanged(); }))
            return false;
    }
    return guard.alive();
}
bool ConfigReadProjectionState::finish() {
    ConfigConsumerGuard guard(q);
    while (!requests.isEmpty()) {
        const auto first = requests.cbegin();
        if (!first->captured)
            break;
        const auto id = first.key();
        auto request = requests.take(id); // Detach before reentry.
        ConfigReadResultData result;
        result.requestId = id;
        result.snapshot = completed;
        result.pending = pending(coveredSequence);
        auto error = request.error;
        if (error.isEmpty() && !live())
            error = "read-owner";
        if (error.isEmpty() &&
            !ConfigObservationAccess::covers(completed.observation(), request.after))
            error = "read-observation";
        result.status = error.isEmpty() ? "observed" : "failed";
        if (!error.isEmpty())
            result.error = {error, {}, "synchronize"};
        const auto immutable = ConfigObservationAccess::result(std::move(result));
        if (!guard.invoke([&] { emit q->completed(immutable); }))
            return false;
        if (request.context && request.callback) {
            if (request.context->thread() == QThread::currentThread()) {
                if (!guard.invoke([&] { request.callback(immutable); }))
                    return false;
            } else
                QMetaObject::invokeMethod(
                    request.context,
                    [context = request.context, callback = request.callback, immutable] {
                        ConfigConsumerGuard(context).invoke([&] { callback(immutable); });
                    },
                    Qt::QueuedConnection);
        }
    }
    return guard.alive();
}
void ConfigReadProjectionState::fail(quint64 requestId, const QString &error) {
    if (!requests.contains(requestId))
        return;
    requests[requestId].captured = true;
    requests[requestId].error = error;
    if (!store || !store->m_publishingModels)
        finish();
}
void ConfigReadProjectionState::invalidate() {
    for (auto &request : requests) {
        request.error = "read-owner";
        request.captured = true;
    }
    if (!store || !store->m_publishingModels)
        finish();
}
bool ConfigReadProjectionState::complete(ConfigStore *store) {
    auto *root = get(store);
    QList<QPointer<ConfigReadProjection>> projections{root};
    projections.append(root->d->views);
    ConfigConsumerGuard guard(store);
    for (const auto &projection : projections)
        if (projection && !guard.invoke([&] { projection->d->prepare(); }))
            return false;
    return guard.alive();
}
bool ConfigReadProjectionState::deliver(ConfigStore *store) {
    auto *root = get(store);
    QList<QPointer<ConfigReadProjection>> projections{root};
    projections.append(root->d->views);
    ConfigConsumerGuard guard(store);
    for (const auto &projection : projections)
        if (projection && !guard.invoke([&] { projection->d->finish(); }))
            return false;
    return guard.alive();
}
bool ConfigReadProjectionState::operation(ConfigReadProjection *projection,
                                          const ConfigCompletion &pair) {
    return ConfigConsumerGuard(projection).invoke([&] {
        emit projection->operationCompleted(pair.result, pair.observation);
    });
}
ConfigReadProjection *ConfigStore::readProjection() { return ConfigReadProjectionState::get(this); }
ConfigReadProjection *ConfigStoreHandle::readProjection() {
    return ConfigReadProjectionState::get(this);
}
ConfigReadProjection *ConfigOverlayEditor::readProjection() {
    return ConfigReadProjectionState::get(this);
}
ConfigReadProjection *ConfigDraft::readProjection() {
    return ConfigReadProjectionState::get(this, false);
}
ConfigReadProjection *ConfigDraft::previewProjection() {
    return ConfigReadProjectionState::get(this, true);
}
ConfigReadSnapshot
ConfigWorker::readSnapshot(const QList<ConfigPath> &paths,
                           const std::shared_ptr<const ConfigSnapshot> &admitted) const {
    ConfigReadSnapshotData data;
    data.schema = m_options.schema;
    data.selection = paths;
    data.admitted = admitted ? admitted : std::make_shared<const ConfigSnapshot>(m_previous);
    ConfigObservationDependency dependency;
    dependency.owner = m_endpoint;
    dependency.incarnation = m_endpoint->order;
    dependency.delivery = m_deliveryGeneration;
    dependency.paths = paths;
    data.observation = ConfigObservationAccess::observation({dependency});
    return ConfigObservationAccess::snapshot(std::move(data));
}
void ConfigWorker::synchronizeRead(const ConfigCommand &command) {
    const auto request = command.read;
    ConfigReadCapture capture;
    capture.projectionId = request->projectionId;
    capture.requestId = request->requestId;
    capture.error = request->selection.error;
    if (!request->selection.lifetime || !*request->selection.lifetime)
        capture.error = "read-owner";
    if (capture.error.isEmpty() && !m_files.isEmpty()) {
        m_reloadSelection = request->selection.leaves;
        const auto fresh = reloadFiles(ConfigCommand{0, m_prefix, "reload", {}});
        m_reloadSelection.clear();
        if (!fresh)
            capture.error = "read-acquisition";
    }
    const auto preview = m_draftPreviews.value(request->previewId);
    if (!request->previewId.isEmpty() &&
        (!preview || preview->lifetime != request->previewLifetime || !*preview->lifetime))
        capture.error = "read-preview";
    ConfigWorkerPacket packet;
    if (capture.error.isEmpty()) {
        packet = publish();
        capture.snapshot = ConfigObservationAccess::select(
            packet.readSnapshot, request->selection.leaves, request->prefix);
        if (preview) {
            auto selection =
                ConfigSelection::capture(m_options.schema, preview->paths, preview->paths,
                                         ConfigSelection::Target::UserLayer, "read-selection");
            selection.lifetime = preview->lifetime;
            const auto candidate =
                resolveCandidate(*m_layers, ConfigEditBatch::capture(selection, preview->edits),
                                 m_listPolicies, m_listMetadata);
            if (!candidate.valid())
                capture.error = candidate.error;
            else {
                auto dependencies =
                    ConfigObservationAccess::dependencies(packet.readSnapshot.observation());
                dependencies.first().paths = preview->paths;
                dependencies.first().previewId = preview->id;
                dependencies.first().previewLifetime = preview->lifetime;
                dependencies.first().previewEpoch = preview->epoch;
                auto snapshot = ConfigObservationAccess::candidate(
                    candidate, m_options.schema,
                    ConfigObservationAccess::observation(dependencies));
                capture.snapshot = ConfigObservationAccess::select(
                    snapshot, request->selection.leaves, request->prefix);
            }
        }
        capture.order = packet.deliveryGeneration;
        capture.coveredSequence = m_prefix;
        if (!ConfigObservationAccess::covers(capture.snapshot.observation(), request->after)) {
            capture.error = "read-observation";
            capture.snapshot = {};
        }
    }
    packet.reads.append(capture);
    post(packet);
}
} // namespace configqml
