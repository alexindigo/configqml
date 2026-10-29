// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configstore.h"
#include "configworker_p.h"
#include "configshutdown_p.h"
#include "configconsumer_p.h"
#include "configlistmodel.h"
#include <QThread>
#include <QUuid>
#include <QElapsedTimer>
#include <QCoreApplication>

namespace configqml {
ConfigStore::ConfigStore(const ConfigStoreOptions &options, QObject *parent)
    : QObject(parent), m_schema(options.schema),
      m_storeId(options.storeId.isEmpty() ? QUuid::createUuid().toString() : options.storeId) {
    m_storeId = QString(m_storeId.constData(), m_storeId.size());
    qRegisterMetaType<ConfigResult>();
    qRegisterMetaType<ConfigInput>();
    if (m_schema.valid())
        for (const auto &path : m_schema.leaves()) {
            const auto descriptor = *m_schema.descriptor(path);
            if (descriptor.type == ConfigDescriptor::Type::List ||
                descriptor.type == ConfigDescriptor::Type::Map) {
                m_models[path] = std::shared_ptr<ConfigListModel>(
                    new ConfigListModel(descriptor, options.modelNumbers));
            }
        }
    auto owned = options;
    owned.evidenceDirectory = ownString(options.evidenceDirectory);
    for (auto &path : owned.observedPaths)
        path = ownPath(path);
    for (auto &projection : owned.projections) {
        projection.projectionId = ownString(projection.projectionId);
        for (auto &id : projection.sourceIds)
            id = ownString(id);
        const auto metadata = ownValue(projection.metadata);
        if (!metadata.ok())
            m_error = "projection-declaration";
        projection.metadata = metadata.value.toMap();
    }
    for (auto &policy : owned.recordListPolicies) {
        policy.policyId = ownString(policy.policyId);
        policy.path = ownPath(policy.path);
        policy.keyField = ownString(policy.keyField);
        for (auto &field : policy.preservedFields)
            field = ownString(field);
        for (auto &selector : policy.listPath)
            selector.key = ownString(selector.key);
        m_policyPaths[policy.policyId] = policy.path;
    }
    for (auto &file : owned.jsonFiles) {
        file.layerId = QString(file.layerId.constData(), file.layerId.size());
        file.sourceId = QString(file.sourceId.constData(), file.sourceId.size());
        file.path = QString(file.path.constData(), file.path.size());
    }
    owned.storeId = QString(m_storeId.constData(), m_storeId.size());
    // Capture borrowed storage before crossing threads. Admission/merging stays in worker.
    for (auto &layer : owned.layers) {
        layer.id = QString(layer.id.constData(), layer.id.size());
        for (auto &source : layer.sources) {
            source.id = QString(source.id.constData(), source.id.size());
            source.fallbackFor = ownString(source.fallbackFor);
            source.prefix = ownPath(source.prefix);
            const auto raw = ownValue(source.raw, source.prefix, m_schema.diagnostics());
            if (raw.ok())
                source.raw = raw.value.toMap();
            else {
                m_error = "unsupported-source";
                source.raw.clear();
            }
            source.bytes = QByteArray(source.bytes.constData(), source.bytes.size());
            source.revision = QByteArray(source.revision.constData(), source.revision.size());
            if (layer.writable && layer.kind == ConfigLayerKind::File) {
                m_userRoutes[source.prefix] = {layer.id, source.id, source.prefix};
                m_userLayerId = layer.id;
            }
        }
    }
    m_owner = std::shared_ptr<ConfigWorkerOwner>(
        new ConfigWorkerOwner(std::move(owned), m_error), [](ConfigWorkerOwner *owner) {
            if (owner->thread() == QThread::currentThread())
                delete owner;
            else
                owner->deleteLater();
        });
    m_worker = m_owner->workerForTest();
    connect(m_owner.get(), &ConfigWorkerOwner::packetReady, this, &ConfigStore::receive);
    connect(m_owner.get(), &ConfigWorkerOwner::joined, this, &ConfigStore::finishThread);
    connect(m_owner.get(), &ConfigWorkerOwner::implicitClose, this,
            [this](const ConfigCommand &command) {
                m_pending.insert(command.requestId, {this, {}});
                m_accepting = false;
                m_status = Status::Closing;
                ConfigConsumerGuard(this).invoke([&] { emit statusChanged(); });
            });
    m_owner->start();
}
ConfigStore::~ConfigStore() {
    // Accepted work belongs to the process-owned worker. Only consumer delivery
    // ends here; no nested drain, abandonment or cancellation of persistence.
    // QObject deletes children after derived members. Close their delivery now
    // so an overlay child's destructor cannot access our destroyed owner member.
    // Worker-owned admitted persistence is still retired by the owner below.
    for (const auto &editor : m_overlays)
        if (editor) {
            editor->m_status = ConfigOverlayEditor::Status::Closed;
            editor->m_store = nullptr;
        }
    m_owner->detach();
}
void ConfigStore::finishThread() {
    m_threadStopped = true;
    m_worker = nullptr;
    if (m_closeResult) {
        const auto result = *m_closeResult;
        m_closeResult.reset();
        receiveResult(result);
    }
}
QVariant ConfigStore::value(const ConfigPath &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_cache.values.value(path);
}
QVariant ConfigStore::presentation(const ConfigPath &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    auto cached = m_presentations.constFind(path);
    if (cached != m_presentations.cend())
        return cached.value();
    const auto descriptor = m_schema.descriptor(path);
    const auto result = descriptor ? collectionPresentation(*descriptor, value(path)) : QVariant();
    m_presentations.insert(ownPath(path), result);
    return result;
}
ConfigPathState ConfigStore::info(const ConfigPath &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_cache.information.value(path);
}
QList<ConfigInput> ConfigStore::inputs() const {
    auto result = m_inputs;
    for (auto &input : result) {
        if (m_schema.sensitive(input.path))
            input.value.clear();
        for (auto it = input.edits.begin(); it != input.edits.end(); ++it)
            if (m_schema.sensitive(it.key()))
                it.value() = QVariant(); // Redacted, not a reset command.
    }
    return result;
}
ConfigListModel *ConfigStore::listModel(const ConfigPath &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    auto *model = m_models.value(path).get();
    return model && model->descriptor().type == ConfigDescriptor::Type::List ? model : nullptr;
}
ConfigListModel *ConfigStore::keyedModel(const ConfigPath &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    auto *model = m_models.value(path).get();
    return model && model->descriptor().type == ConfigDescriptor::Type::Map ? model : nullptr;
}
bool ConfigStore::canWriteUser(const ConfigPath &path) const {
    for (qsizetype size = path.size(); size >= 0; --size)
        if (m_userRoutes.contains(path.mid(0, size)))
            return true;
    return false;
}
quint64 ConfigStore::registerRequest(QObject *context, Callback callback) {
    const auto id = m_owner->reserveRequest();
    m_pending.insert(id, {context ? context : this, std::move(callback)});
    return id;
}
ConfigRequestHandle ConfigStore::write(ConfigInput::Kind kind, const ConfigPath &queryPath,
                                       const QVariant &value, const QString &target,
                                       QObject *context, Callback callback,
                                       ConfigDescriptor::Format format,
                                       const ConfigAdmission *admission) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto path = ownPath(queryPath);
    const auto diagnostics = m_schema.diagnostics();
    ConfigRequestHandle handle;
    ConfigConsumerGuard boundary(this);
    auto checked = admission ? m_schema.admitted(*admission)
                   : (kind == ConfigInput::Kind::User || kind == ConfigInput::Kind::Runtime ||
                      kind == ConfigInput::Kind::Initialize)
                       ? m_schema.validate(path, value, format)
                       : ConfigValueResult{};
    if (!m_accepting || !m_owner->accepting() || m_status != Status::Ready)
        checked.errors = {diagnostics.make(
            path, m_accepting && m_owner->accepting() ? "not-ready" : "store-closed")};
    if (!m_error.isEmpty())
        checked.errors = {diagnostics.make(path, m_error)};
    if ((kind == ConfigInput::Kind::Reset || kind == ConfigInput::Kind::ClearRuntime) &&
        !m_schema.descriptor(path))
        checked.errors = {diagnostics.make(path, "undeclared-path")};
    if (checked.ok() &&
        (kind == ConfigInput::Kind::User || kind == ConfigInput::Kind::Initialize) &&
        !canWriteUser(path))
        checked.errors = {diagnostics.make(path, "read-only-path")};
    if (checked.ok() && kind == ConfigInput::Kind::Reset) {
        const auto route = m_userRoutes.lowerBound(path);
        const bool coversSource = route != m_userRoutes.cend() && startsWith(route.key(), path);
        if (target != m_userLayerId || (!canWriteUser(path) && !coversSource))
            checked.errors = {diagnostics.make(path, "read-only-path")};
    }
    if (checked.ok() && kind != ConfigInput::Kind::Runtime &&
        kind != ConfigInput::Kind::ClearRuntime)
        for (const auto &source : m_sourceStates) {
            bool writable = false;
            for (const auto &route : m_userRoutes)
                writable |= route.sourceId == source.sourceId;
            if (source.quarantined && writable &&
                (startsWith(path, source.prefix) || startsWith(source.prefix, path)))
                checked.errors = {diagnostics.make(path, "source-quarantined")};
        }
    handle.requestId = registerRequest(context, std::move(callback));
    if (!checked.ok()) {
        handle.errors = checked.errors;
        ConfigResult result;
        result.requestId = handle.requestId;
        result.storeId = m_storeId;
        result.operation = "write";
        result.status = "failed";
        result.inputPrefix = m_sequence;
        result.error = diagnostics.error(checked.errors.first(), {}, "ingress");
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
        boundary.invoke([&] { emit inputRejected(diagnostics.path(path), result.error.code); });
        return handle;
    }
    ConfigInput input;
    input.kind = kind;
    input.sequence = m_sequence + 1;
    input.path = ownPath(path);
    input.value = checked.value;
    input.target = QString(target.constData(), target.size());
    const auto affected = kind == ConfigInput::Kind::User || kind == ConfigInput::Kind::Runtime ||
                                  kind == ConfigInput::Kind::Initialize
                              ? QList<ConfigPath>{path}
                              : m_schema.leaves();
    for (const auto &leaf : affected)
        if (startsWith(leaf, path)) {
            input.pathGenerations[leaf] = m_cache.information.value(leaf).pathGeneration + 1;
        }
    ConfigCommand command{handle.requestId, input.sequence, "write", input};
    if (!m_owner->dispatch(command)) {
        handle.errors = {diagnostics.make(path, "store-closed")};
        ConfigResult result;
        result.requestId = handle.requestId;
        result.storeId = m_storeId;
        result.operation = "write";
        result.status = "failed";
        result.inputPrefix = m_sequence;
        result.error = {"store-closed", {}, "ingress"};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
        boundary.invoke([&] { emit inputRejected(diagnostics.path(path), result.error.code); });
        return handle;
    }
    m_sequence = input.sequence;
    for (auto it = input.pathGenerations.cbegin(); it != input.pathGenerations.cend(); ++it) {
        auto &state = m_cache.information[it.key()];
        state.latestInput = input.sequence;
        state.pathGeneration = it.value();
    }
    m_inputs.append(input);
    handle.inputSequence = input.sequence;
    handle.accepted = true;
    bool changed = false;
    if (kind == ConfigInput::Kind::User && info(path).provenance.maskedBy.isEmpty()) {
        changed = m_cache.values.value(path) != checked.value;
        m_cache.values[path] = checked.value;
        if (changed)
            m_presentations.remove(path);
    }
    // Owned input, sequence, queueing and cache installation all precede consumer reentry.
    if (changed)
        boundary.invoke([&] { emit valuesChanged({path}); });
    return handle;
}
ConfigRequestHandle ConfigStore::setUser(const ConfigAdmission &value, QObject *context,
                                         Callback callback) {
    return write(ConfigInput::Kind::User, value.path(), {}, {}, context, std::move(callback),
                 ConfigDescriptor::Format::Native, &value);
}
ConfigRequestHandle ConfigStore::setRuntime(const ConfigAdmission &value, QObject *context,
                                            Callback callback) {
    return write(ConfigInput::Kind::Runtime, value.path(), {}, {}, context, std::move(callback),
                 ConfigDescriptor::Format::Native, &value);
}
ConfigRequestHandle ConfigStore::initializeIfMissing(const ConfigAdmission &value, QObject *context,
                                                     Callback callback) {
    return write(ConfigInput::Kind::Initialize, value.path(), {}, {}, context, std::move(callback),
                 ConfigDescriptor::Format::Native, &value);
}
ConfigRequestHandle ConfigStore::setUser(const ConfigPath &p, const QVariant &v, QObject *c,
                                         Callback f, ConfigDescriptor::Format format) {
    return write(ConfigInput::Kind::User, p, v, {}, c, std::move(f), format);
}
ConfigRequestHandle ConfigStore::initializeIfMissing(const ConfigPath &p, const QVariant &v,
                                                     QObject *c, Callback f,
                                                     ConfigDescriptor::Format format) {
    return write(ConfigInput::Kind::Initialize, p, v, {}, c, std::move(f), format);
}
ConfigRequestHandle ConfigStore::setRuntime(const ConfigPath &p, const QVariant &v, QObject *c,
                                            Callback f, ConfigDescriptor::Format format) {
    return write(ConfigInput::Kind::Runtime, p, v, {}, c, std::move(f), format);
}
ConfigRequestHandle ConfigStore::reset(const ConfigPath &p, const QString &t, QObject *c,
                                       Callback f) {
    return write(ConfigInput::Kind::Reset, p, {}, t, c, std::move(f));
}
ConfigRequestHandle ConfigStore::clearRuntime(const ConfigPath &p, QObject *c, Callback f) {
    return write(ConfigInput::Kind::ClearRuntime, p, {}, {}, c, std::move(f));
}
quint64 ConfigStore::request(const QString &operation, QObject *context, Callback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto id = registerRequest(context, std::move(callback));
    ConfigCommand command{id, m_sequence, operation, {}};
    if (!m_accepting || !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = operation;
        result.status = "failed";
        result.inputPrefix = m_sequence;
        result.error = {"store-closed", {}, operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    } else {
        if (operation == "close") {
            m_accepting = false;
            m_status = Status::Closing;
        }
        if (operation == "close")
            ConfigConsumerGuard(this).invoke([&] { emit statusChanged(); });
    }
    return id;
}
quint64 ConfigStore::save(QObject *c, Callback f) { return request("save", c, std::move(f)); }
quint64 ConfigStore::reload(QObject *c, Callback f) { return request("reload", c, std::move(f)); }
quint64 ConfigStore::flush(QObject *c, Callback f) { return request("flush", c, std::move(f)); }
quint64 ConfigStore::close(QObject *c, Callback f) { return request("close", c, std::move(f)); }
void ConfigStore::receive(const ConfigWorkerPacket &packet) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_publishingModels) {
        QMetaObject::invokeMethod(this, [this, packet] { receive(packet); }, Qt::QueuedConnection);
        return;
    }
    QElapsedTimer publicationTimer;
    publicationTimer.start();
    if (!packet.models.isEmpty())
        m_lastPlanningNanos = packet.planningNanos;
    QList<ConfigPath> changed;
    QMap<ConfigPath, ConfigModelPlan> modelPlans;
    if (packet.snapshot.topology == m_cache.topology) {
        if (packet.admitted &&
            (!m_admittedSnapshot || packet.admitted->generation > m_admittedSnapshot->generation)) {
            auto previous = std::move(m_admittedSnapshot);
            m_admittedSnapshot = packet.admitted;
            // Reclaim replaced full snapshots on their worker, not during a GUI
            // delta publication. Facade maps never share their mutable containers.
            if (previous)
                m_owner->reclaim(std::move(previous));
        }
        for (auto it = packet.snapshot.information.cbegin();
             it != packet.snapshot.information.cend(); ++it) {
            auto &state = m_cache.information[it.key()];
            if (it->workerCovered < state.latestInput ||
                it->pathGeneration != state.pathGeneration ||
                it->workerCovered < state.workerCovered ||
                it->publicationGeneration <= state.publicationGeneration)
                continue;
            const auto next = packet.snapshot.values.value(it.key());
            if (m_cache.values.value(it.key()) != next) {
                changed.append(it.key());
                m_presentations.remove(it.key());
            }
            m_cache.values[it.key()] = next;
            const auto latest = state.latestInput;
            state = it.value();
            state.latestInput = qMax(latest, it->latestInput);
            if (auto *model = m_models.value(it.key()).get()) {
                const auto plans = packet.models.value(it.key());
                const auto base = model->m_generation;
                if (plans.contains(base))
                    modelPlans[it.key()] = plans.value(base);
            }
        }
        m_cache.generation = qMax(m_cache.generation, packet.snapshot.generation);
    }
    bool statusChange = false;
    if (packet.initial && m_status == Status::Loading) {
        m_status = packet.error.isEmpty() && m_error.isEmpty() ? Status::Ready : Status::Error;
        if (!packet.error.isEmpty())
            m_error = packet.error;
        statusChange = true;
    }
    if (packet.terminal) {
        m_status = Status::Error;
        m_error = packet.error;
        statusChange = true;
    }
    // Complete generation is installed before the first property notification.
    m_groups = packet.groups;
    // Published display state is not ingress authority. Keep monotone worker
    // observations separate from claims made before their replies can arrive.
    for (auto it = packet.groups.cbegin(); it != packet.groups.cend(); ++it) {
        auto &ingress = m_retainedIngress[it.key()];
        const auto generation = packet.groupGenerations.value(it.key());
        if (generation >= ingress.generation) {
            ingress.generation = generation;
            ingress.state = it->state;
            ingress.capturePrefix = it->capturePrefix;
        }
    }
    if (packet.result)
        for (auto &ingress : m_retainedIngress)
            if (ingress.pendingRequest && ingress.pendingRequest == packet.result->requestId) {
                ingress.pendingRequest = ingress.pendingGeneration = 0;
                ingress.pendingState.clear();
            }
    ConfigConsumerGuard boundary(this);
    for (const auto &p : packet.overlays) {
        if (packet.result && packet.result->operation == "openOverlay" &&
            packet.result->status == "opened")
            m_overlayPresentations[packet.result->requestId] =
                std::make_shared<ConfigOverlayPresentation>(p);
        else if (!boundary.invoke([&] { receiveOverlay(p); }))
            return;
    }
    m_publishingModels = true;
    QMap<ConfigPath, quint64> acknowledgements;
    qint64 modelNotifications = 0;
    for (auto it = modelPlans.cbegin(); it != modelPlans.cend(); ++it) {
        const auto native = m_models.value(it.key());
        if (native) {
            if (!boundary.invoke([&] { native->apply(it.value(), this); }))
                return;
            modelNotifications += native->m_lastNotificationNanos;
            acknowledgements[it.key()] = it->generation;
        }
    }
    m_publishingModels = false;
    m_owner->acknowledgeModels(acknowledgements);
    if (packet.admitted && !boundary.invoke([&] { emit published(); }))
        return;
    if (!changed.isEmpty() && !boundary.invoke([&] { emit valuesChanged(changed); }))
        return;
    if (packet.sourceStates) {
        m_sourceStates = *packet.sourceStates;
        if (!boundary.invoke([&] { emit sourceStatesChanged(); }))
            return;
    }
    for (const auto &event : packet.projectionEffects)
        if (!boundary.invoke([&] { emit projectionEffects(event); }))
            return;
    for (const auto &diagnostic : packet.projectionDiagnostics)
        if (!boundary.invoke(
                [&] { emit sourceWarning(m_schema.diagnostics().sanitize(diagnostic)); }))
            return;
    if (packet.sourceAccepted &&
        !boundary.invoke([&] { emit sourceAccepted(*packet.sourceAccepted); }))
        return;
    if (statusChange && !boundary.invoke([&] { emit statusChanged(); }))
        return;
    if (packet.result) {
        if (packet.result->operation == "close" && !m_threadStopped) {
            m_worker = nullptr;
            m_closeResult = packet.result;
        } else if (!boundary.invoke([&] { receiveResult(*packet.result); }))
            return;
    }
    if (packet.sourceError && !boundary.invoke([&] {
            emit sourceError(m_schema.diagnostics().sanitize(*packet.sourceError));
        }))
        return;
    if (packet.migrationResult &&
        !boundary.invoke([&] { emit migrationCompleted(*packet.migrationResult); }))
        return;
    if (packet.autoSaveRequested && m_accepting && m_status == Status::Ready)
        request("autosave", this, {});
    m_worstPublicationNanos = qMax(m_worstPublicationNanos, publicationTimer.nsecsElapsed());
    if (!packet.models.isEmpty())
        m_lastLibraryPublicationNanos = publicationTimer.nsecsElapsed() - modelNotifications;
}
void ConfigStore::receiveResult(const ConfigResult &input) {
    const auto result = m_schema.diagnostics().sanitize(input);
    if (result.requestId < m_nextCompletion || !m_pending.contains(result.requestId))
        return;
    if (result.operation == "retainGroup" && result.status == "failed") {
        for (auto it = m_retainedRequests.begin(); it != m_retainedRequests.end();)
            if (it.value() == result.requestId)
                it = m_retainedRequests.erase(it);
            else
                ++it;
    }
    m_results[result.requestId] = result;
    deliver();
}
void ConfigStore::deliver() {
    if (m_delivering)
        return;
    m_delivering = true;
    ConfigConsumerGuard boundary(this);
    while (m_results.contains(m_nextCompletion)) {
        const auto result = m_results.take(m_nextCompletion);
        auto pending = m_pending.take(m_nextCompletion++); // Detach before arbitrary reentry.
        if (result.operation == "openOverlay") {
            const auto presentation = m_overlayPresentations.take(result.requestId);
            if (result.status == "opened" && presentation && pending.context) {
                auto *editor = new ConfigOverlayEditor(this, *presentation);
                m_overlays[result.editorId] = editor;
                if (!boundary.invoke([&] { emit overlayOpened(result.requestId, editor); }))
                    return;
            } else {
                const auto endpoint = m_owner->overlayEndpoint();
                overlayRemoveDependency(endpoint, result.editorId);
                if (result.status == "opened")
                    endpoint->send(
                        [id = result.editorId](ConfigWorker &w) { w.overlayDetach(id); });
            }
        }
        if (result.status == "failed")
            m_failures.append(result);
        if (result.operation == "close") {
            m_status = Status::Closed;
            m_worker = nullptr;
            if (!boundary.invoke([&] { emit statusChanged(); }))
                return;
        }
        if (!boundary.invoke([&] { emit completed(result); }))
            return;
        if (result.operation == "autosave" && !boundary.invoke([&] { emit autoSaved(result); }))
            return;
        if (pending.context && pending.callback) {
            if (pending.context->thread() == QThread::currentThread())
                boundary.invoke([&] { pending.callback(result); });
            else
                QMetaObject::invokeMethod(
                    pending.context,
                    [context = pending.context, callback = std::move(pending.callback), result] {
                        ConfigConsumerGuard(context).invoke([&] { callback(result); });
                    },
                    Qt::QueuedConnection);
        }
        if (!boundary.alive())
            return;
    }
    m_delivering = false;
}
} // namespace configqml
