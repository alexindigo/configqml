// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "configevidence_p.h"
#include <QElapsedTimer>
#include "confignumber.h"
#include <QDataStream>
#include <QIODevice>
#include <QThread>
#include <QFileInfo>
#include <QDir>
#include <exception>

namespace configqml {
// Memory-only envelope, not a file format. Every node is tagged so unknown map
// keys cannot collide with the numeric-token representation. URLs and all 64-bit
// domains retain native types. M3 supplies a separate JSON codec/backend.
static QVariant envelope(const QVariant &value, bool decode) {
    if (decode) {
        const auto node = value.toList();
        if (node.size() != 2)
            return {};
        const auto tag = node[0].toString();
        if (tag == "number") {
            auto number = ConfigNumber::fromJson(node[1].toString());
            return number ? QVariant::fromValue(*number) : QVariant();
        }
        if (tag == "list") {
            QVariantList list;
            for (const auto &item : node[1].toList())
                list.append(envelope(item, true));
            return list;
        }
        if (tag == "map") {
            auto map = node[1].toMap();
            for (auto &item : map)
                item = envelope(item, true);
            return map;
        }
        return tag == "value" ? node[1] : QVariant();
    }
    if (value.metaType() == QMetaType::fromType<ConfigNumber>())
        return QVariantList{QString("number"), value.value<ConfigNumber>().token()};
    if (value.metaType().id() == QMetaType::QVariantList) {
        QVariantList list;
        for (const auto &item : value.toList())
            list.append(envelope(item, false));
        return QVariantList{QString("list"), list};
    }
    if (value.metaType().id() == QMetaType::QVariantMap) {
        auto map = value.toMap();
        for (auto &item : map)
            item = envelope(item, false);
        return QVariantList{QString("map"), map};
    }
    return QVariantList{QString("value"), value};
}
QByteArray encodeMemoryValue(const QVariantMap &raw) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << envelope(raw, false);
    return stream.status() == QDataStream::Ok ? bytes : QByteArray();
}
QVariantMap decodeMemoryValue(const QByteArray &bytes) {
    QDataStream stream(bytes);
    QVariant value;
    stream >> value;
    return stream.status() == QDataStream::Ok ? envelope(value, true).toMap() : QVariantMap();
}
ConfigWorker::ConfigWorker(ConfigStoreOptions options, QString initializationError)
    : m_options(std::move(options)), m_initializationError(std::move(initializationError)) {}
void ConfigWorker::initialize() {
    Q_ASSERT(QThread::currentThread() == thread());
    auto topology = m_options.layers;
    for (auto &layer : topology)
        for (auto &source : layer.sources) {
            source.raw.clear();
            source.exists = false;
            source.localIntent = false;
        }
    const ConfigLayers declarations(m_options.schema, topology, m_options.projections);
    if (!declarations.valid() && m_initializationError.isEmpty())
        m_initializationError = declarations.errors().first().code;
    m_backend = std::make_unique<ConfigMemoryBackend>();
    if (!m_options.evidenceDirectory.isEmpty() &&
        (!QDir::isAbsolutePath(m_options.evidenceDirectory) ||
         QDir::cleanPath(m_options.evidenceDirectory) != m_options.evidenceDirectory ||
         (QFileInfo::exists(m_options.evidenceDirectory) &&
          !QFileInfo(m_options.evidenceDirectory).isDir()) ||
         QFileInfo(m_options.evidenceDirectory).isSymLink()))
        m_initializationError = "evidence-directory";
    QSet<ConfigPath> observed;
    for (const auto &path : m_options.observedPaths) {
        const auto descriptor = m_options.schema.descriptor(path);
        if (!descriptor || descriptor->type == ConfigDescriptor::Type::Group ||
            observed.contains(path))
            m_initializationError = "observed-path";
        observed.insert(path);
    }
    for (const auto &policy : m_options.recordListPolicies) {
        auto error = validateRecordListPolicy(m_options.schema, policy);
        if (m_listPolicies.contains(policy.policyId))
            error = "list-policy-declaration";
        for (const auto &other : m_listPolicies) {
            if (other.path != policy.path)
                continue;
            bool overlap = true;
            for (qsizetype i = 0; i < qMin(other.listPath.size(), policy.listPath.size()); ++i) {
                const auto a = other.listPath[i], b = policy.listPath[i];
                if (!(a == b) && a.kind != ConfigValueSelector::Kind::Each &&
                    b.kind != ConfigValueSelector::Kind::Each)
                    overlap = false;
            }
            if (overlap)
                error = "list-policy-declaration";
        }
        if (!error.isEmpty())
            m_initializationError = error;
        m_listPolicies.insert(policy.policyId, policy);
    }
    QMap<QString, ConfigDiagnosticBuilder> diagnostics;
    for (const auto &layer : m_options.layers)
        for (const auto &source : layer.sources)
            diagnostics[source.id] = m_options.schema.diagnostics().at(source.prefix);
    m_json =
        std::make_unique<ConfigJsonBackend>(m_options.jsonFiles, m_options.fileFault, diagnostics);
    QSet<QString> paths;
    QMap<QString, int> sourceIds;
    for (const auto &layer : m_options.layers)
        for (const auto &source : layer.sources)
            ++sourceIds[source.id];
    for (const auto &file : m_options.jsonFiles) {
        ConfigSourceState state;
        state.sourceId = file.sourceId;
        state.path = file.path;
        m_sourceStates.append(state);
        if ((file.startupErrorPolicy == ConfigStartupErrorPolicy::SkipProjectionRetainError ||
             file.conflictPolicy == ConfigConflictPolicy::StrictSourcePreimage) &&
            m_options.evidenceDirectory.isEmpty())
            m_initializationError = "evidence-directory";
        bool found = false;
        for (auto &layer : m_options.layers)
            if (layer.id == file.layerId && layer.kind == ConfigLayerKind::File)
                for (auto &source : layer.sources)
                    if (source.id == file.sourceId) {
                        found = true;
                        if (file.deleteWhenEmpty && !layer.writable)
                            m_initializationError = "empty-delete-readonly";
                        if (sourceIds.value(file.sourceId) != 1 ||
                            m_files.contains(file.sourceId) ||
                            paths.contains(m_json->path(file.sourceId)))
                            m_initializationError = "duplicate-file-identity";
                        paths.insert(m_json->path(file.sourceId));
                        m_files[file.sourceId] = file;
                        m_fileRoutes[file.sourceId] = {layer.id, source.id, source.prefix};
                        m_sourceStates.last().prefix = source.prefix;
                        m_sourceStates.last().fallbackFor = source.fallbackFor;
                        source.raw.clear();
                        source.exists = false;
                        source.json = true;
                        source.pruneResetNamespaces = file.deleteWhenEmpty;
                        if (file.watchMode == ConfigWatchMode::Off)
                            continue;
                        const auto id = m_json->submit(
                            {ConfigBackendRequest::Operation::Read, file.sourceId, {}, {}});
                        auto read = *m_json->take(id);
                        ConfigJsonCodec codec(m_options.schema.diagnostics().at(source.prefix));
                        const auto decoded = read.revision.exists
                                                 ? codec.decode(read.bytes, file.sourceId)
                                                 : ConfigDecodeResult{};
                        auto sourceFailure = !read.success ? read.error : decoded.error;
                        if (read.success && !read.revision.exists && !file.optional)
                            sourceFailure = {"required-file-missing", file.sourceId, "read"};
                        observeSource(file.sourceId, read, sourceFailure, true);
                        if (!read.success || !decoded.ok() ||
                            (!read.revision.exists && !file.optional)) {
                            const auto failure = !read.success   ? read.error.code
                                                 : !decoded.ok() ? decoded.error.code
                                                                 : "required-file-missing";
                            if (file.startupErrorPolicy == ConfigStartupErrorPolicy::FailReady ||
                                (read.success && decoded.ok()))
                                if (m_initializationError.isEmpty())
                                    m_initializationError = failure;
                            source.available = false;
                            m_sourceStates.last().quarantined =
                                file.startupErrorPolicy ==
                                ConfigStartupErrorPolicy::SkipProjectionRetainError;
                            auto packet = ConfigWorkerPacket{};
                            auto error = !read.success ? read.error : decoded.error;
                            if (error.empty())
                                error = {"required-file-missing", file.sourceId, "read"};
                            error.file = m_json->path(file.sourceId);
                            packet.sourceError = error;
                            post(packet);
                            continue;
                        }
                        source.raw = decoded.raw;
                        source.exists = read.revision.exists;
                        source.bytes = read.bytes;
                        source.revision = read.revision.token;
                        source.diskExists = read.revision.exists;
                        m_baselines[file.sourceId] = source.raw;
                        m_revisions[file.sourceId] = read.revision;
                        m_sourceBytes[file.sourceId] = read.bytes;
                        m_json->confirm(file.sourceId, read.revision);
                    }
        if (!found)
            m_initializationError = "unknown-file-route";
    }
    migrateSources();
    m_layers =
        std::make_unique<ConfigLayers>(m_options.schema, m_options.layers, m_options.projections);
    m_diskLayers = std::make_unique<ConfigLayers>(*m_layers);
    m_diskLayers->removeTransientLayers();
    m_diskLayers->takeProjectionEffects();
    m_diskLayers->takeProjectionDiagnostics();
    m_acceptOrigin = "startup";
    m_acceptCause = "startup";
    if (m_initializationError.isEmpty() && !m_layers->rejectedDiagnostics().isEmpty()) {
        const auto code = m_layers->rejectedDiagnostics().first().code;
        m_initializationError =
            code.startsWith("projection-") ? code : QString("strict-file-admission");
    }
    for (const auto &layer : m_options.layers) {
        if (!layer.writable)
            continue;
        for (const auto &source : layer.sources) {
            // Backends are per logical store; source identity is unique within its user layer.
            m_routes[source.id] = {layer.id, source.id, source.prefix};
            if (m_files.contains(source.id))
                continue;
            m_backend->seed(source.id, encodeMemoryValue(source.raw), source.exists);
            m_baselines[source.id] = source.raw;
            auto id = m_backend->submit({ConfigBackendRequest::Operation::Read, source.id, {}, {}});
            m_revisions[source.id] = m_backend->take(id)->revision;
        }
    }
    m_options.layers.clear();
    m_autoTimer = new QTimer(this);
    m_autoTimer->setSingleShot(true);
    connect(m_autoTimer, &QTimer::timeout, this, [this] {
        if (m_closed || m_failed || m_dirty.isEmpty())
            return;
        ConfigWorkerPacket packet;
        packet.autoSaveRequested = true;
        post(packet);
    });
    m_watcher = new QFileSystemWatcher(this);
    m_watchTimer = new QTimer(this);
    m_watchTimer->setSingleShot(true);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_watchTimer->start(20); });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { m_watchTimer->start(20); });
    connect(m_watchTimer, &QTimer::timeout, this, [this] {
        if (m_busy || m_holdInputs) {
            m_watchTimer->start(20);
            return;
        }
        if (!m_closed && !m_failed)
            reloadFiles();
    });
    armWatches();
    auto packet = publish({}, true);
    if (!m_layers->valid() || !m_initializationError.isEmpty()) {
        m_failed = true;
        packet.error = !m_initializationError.isEmpty() ? m_initializationError
                                                        : m_layers->errors().first().code;
    }
    post(packet);
}
ConfigResult ConfigWorker::resultFor(const ConfigCommand &command) const {
    ConfigResult result;
    result.requestId = command.requestId;
    result.storeId = m_options.storeId;
    result.operation = command.operation;
    result.inputPrefix = command.prefix;
    return result;
}
bool ConfigWorker::inheritFailures(ConfigResult &result) const {
    bool found = false;
    for (const auto &failure : m_failures) {
        if (failure.inputPrefix > result.inputPrefix)
            continue;
        found = true;
        result.status = "failed";
        if (result.error.empty())
            result.error = failure.error;
        result.publications.append(failure.publications);
        result.evidence.append(failure.evidence);
        for (auto it = failure.coveredRevisions.cbegin(); it != failure.coveredRevisions.cend();
             ++it)
            result.coveredRevisions[it.key()] = it.value();
        if (failure.publicationState == "partial" ||
            (failure.publicationState == "uncertain" && result.publicationState == "known"))
            result.publicationState = failure.publicationState;
    }
    return found;
}
ConfigWorkerPacket ConfigWorker::publish(const std::optional<ConfigResult> &result, bool initial) {
    ConfigWorkerPacket packet;
    for (const auto &capture : m_groups) {
        auto info = capture.info;
        for (auto &attempt : info.attempts)
            attempt.error = m_options.schema.diagnostics().sanitize(attempt.error);
        packet.groups[info.groupToken] = info;
        packet.groupGenerations[info.groupToken] = capture.generation;
    }
    packet.initial = initial;
    packet.overlays = std::move(m_overlayPackets);
    m_overlayPackets.clear();
    packet.result = result;
    packet.snapshot.generation = ++m_generation;
    packet.snapshot.inputPrefix = m_prefix;
    auto effective = m_layers->effective(); // One full-tree owner, never the GUI.
    QSet<ConfigPath> listPaths;
    for (const auto &policy : m_listPolicies)
        listPaths.insert(policy.path);
    for (const auto &path : listPaths) {
        const auto value = m_layers->unmaskedValue(path);
        if (!value)
            continue;
        QStringList revisions;
        for (const auto &policy : m_listPolicies)
            if (policy.path == path)
                revisions.append(QString::number(m_listRevisions.value(policy.policyId)));
        if (!m_listInputs.contains(path) || m_listInputs.value(path) != *value ||
            m_listMetadataKeys.value(path) != revisions) {
            QVariant projectedValue = *value;
            QString error;
            for (const auto &policy : m_listPolicies) {
                if (policy.path != path)
                    continue;
                const auto projected = projectRecordLists(
                    policy, m_listMetadata.value(policy.policyId), projectedValue);
                if (!projected.error.isEmpty()) {
                    error = projected.error;
                    break;
                }
                projectedValue = projected.value;
            }
            if (!error.isEmpty()) {
                packet.error = error;
                packet.terminal = !initial;
            } else {
                m_listInputs[path] = *value;
                m_listMetadataKeys[path] = revisions;
                m_listOutputs[path] = projectedValue;
            }
        }
        if (m_layers->info(path).maskedBy.isEmpty() && m_listOutputs.contains(path))
            putValue(effective, path, m_listOutputs.value(path));
    }
    for (const auto &path : m_options.schema.leaves()) {
        const auto next = valueAt(effective, path).value_or(QVariant());
        ConfigPathState state;
        state.provenance = m_layers->info(path);
        state.workerCovered = qMax(m_prefix, m_pathInputs.value(path));
        state.latestInput = m_pathInputs.value(path);
        state.intentRevision = m_pathIntents.value(path);
        QVariantList content{next, QString::number(state.intentRevision)};
        for (const auto &policy : m_listPolicies)
            if (policy.path == path)
                content.append(QString::number(m_listRevisions.value(policy.policyId)));
        for (const auto &contribution : state.provenance.contributions) {
            const auto source = m_layers->source(contribution.layerId, contribution.sourceId);
            if (!source)
                continue;
            const auto raw = valueAt(source->raw, path.mid(source->prefix.size()));
            content.append(
                QVariantMap{{"present", bool(raw)}, {"value", raw.value_or(QVariant())}});
        }
        if (!m_content.contains(path) || m_content.value(path) != content) {
            ++m_contentRevisions[path];
            m_content[path] = content;
        }
        state.contentRevision = m_contentRevisions.value(path);
        state.pathGeneration = m_pathGenerations.value(path);
        state.publicationGeneration = packet.snapshot.generation;
        const auto before = m_previous.information.value(path);
        if (initial || next != m_previous.values.value(path) ||
            state.pathGeneration != before.pathGeneration ||
            state.provenance.layerId != before.provenance.layerId ||
            state.provenance.maskedBy != before.provenance.maskedBy ||
            state.provenance.contributions != before.provenance.contributions ||
            state.contentRevision != before.contentRevision) {
            packet.snapshot.values[path] = next;
            packet.snapshot.information[path] = state;
            const auto descriptor = *m_options.schema.descriptor(path);
            if (descriptor.type == ConfigDescriptor::Type::List ||
                descriptor.type == ConfigDescriptor::Type::Map) {
                auto &versions = m_modelVersions[path];
                QElapsedTimer planningTimer;
                planningTimer.start();
                if (versions.isEmpty())
                    versions[0] = {};
                const auto rows = projectionRows(descriptor, next);
                for (auto it = versions.cbegin(); it != versions.cend(); ++it) {
                    auto plan = reconcileProjection(descriptor, it.value(), rows);
                    plan.base = it.key();
                    plan.generation = packet.snapshot.generation;
                    packet.models[path][it.key()] = std::move(plan);
                }
                versions[packet.snapshot.generation] = rows;
                packet.planningNanos += planningTimer.nsecsElapsed();
            }
        } else
            state.publicationGeneration = before.publicationGeneration;
        m_previous.values[path] = next;
        m_previous.information[path] = state;
    }
    m_previous.generation = m_generation;
    m_previous.inputPrefix = m_prefix;
    overlayNotify();
    packet.admitted = std::make_shared<const ConfigSnapshot>(m_previous);
    packet.sourceStates = m_sourceStates;
    for (auto &state : *packet.sourceStates)
        state.fallbackState = m_layers->fallbackState(state.sourceId);
    packet.projectionEffects = m_layers->takeProjectionEffects();
    for (const auto &diagnostic : m_layers->takeProjectionDiagnostics())
        packet.projectionDiagnostics.append(m_options.schema.diagnostics().error(
            diagnostic.second, diagnostic.first, "projection-warning"));
    if (!m_acceptOrigin.isEmpty() && m_diskLayers) {
        ConfigSourceAccepted event;
        event.eventId = QString::number(++m_sourceEventId);
        event.publicationGeneration = m_generation;
        event.inputPrefix = m_prefix;
        event.origin = std::move(m_acceptOrigin);
        event.cause = std::move(m_acceptCause);
        m_acceptOrigin.clear();
        m_acceptCause.clear();
        for (const auto &state : m_sourceStates)
            event.sources.append({state.sourceId, state.revision});
        for (const auto &path : m_options.schema.leaves()) {
            const auto value = m_diskLayers->value(path);
            const auto info = m_diskLayers->info(path);
            QByteArray revision;
            for (const auto &contribution : info.contributions)
                if (contribution.admitted && contribution.maskedBy.isEmpty())
                    revision = contribution.revision;
            const QVariantList fact{bool(value), value.value_or(QVariant()), info.layerId,
                                    info.sourceId, revision};
            if (!m_diskFacts.contains(path) || m_diskFacts.value(path) != fact ||
                (event.origin == "projection" && packet.snapshot.information.contains(path)))
                event.changedPaths.append(path);
            m_diskFacts[path] = fact;
            if (m_options.observedPaths.contains(path))
                event.facts.append(
                    {path,
                     bool(value),
                     value.value_or(QVariant()),
                     value ? (info.layerId == "defaults" ? QString("defaults") : info.sourceId)
                           : QString(),
                     {bool(value), revision}});
        }
        if (initial || !event.changedPaths.isEmpty() || event.origin == "projection")
            packet.sourceAccepted = event;
    }
    return packet;
}
QString ConfigWorker::quarantined(const ConfigPath &path) const {
    for (const auto &state : m_sourceStates)
        if (state.quarantined && m_routes.contains(state.sourceId) &&
            (startsWith(path, state.prefix) || startsWith(state.prefix, path)))
            return "source-quarantined";
    return {};
}
QString ConfigWorker::observeSource(const QString &id, const ConfigBackendResult &read,
                                    const ConfigError &error, bool startup) {
    QString retentionError;
    for (auto &state : m_sourceStates) {
        if (state.sourceId != id)
            continue;
        state.presence = read.observedPresence ? (*read.observedPresence ? "present" : "absent")
                         : QFileInfo::exists(m_json->path(id)) ? "present"
                                                               : "unknown";
        state.load = !read.success           ? "io_error"
                     : !read.revision.exists ? "missing"
                     : !error.empty()        ? "parse_error"
                                             : "loaded";
        state.diagnostic = error;
        if (error.empty() && read.success) {
            state.revision = read.revision;
            state.quarantined = false;
        }
    }
    if (startup) {
        retentionError = retainObservation(id, read, "read", "startup");
        if (!retentionError.isEmpty() && m_initializationError.isEmpty())
            m_initializationError = retentionError;
    }
    return retentionError;
}
QString ConfigWorker::retainObservation(const QString &id, const ConfigBackendResult &read,
                                        const QString &phase, const QString &role,
                                        const std::optional<ConfigCommand> &command,
                                        ConfigResult *result) {
    if (m_options.evidenceDirectory.isEmpty())
        return {};
    ConfigEvidenceObservation observation;
    observation.storeId = m_options.storeId;
    observation.sourceId = id;
    observation.phase = phase;
    observation.role = role;
    observation.presence = read.observedPresence ? (*read.observedPresence ? "present" : "absent")
                           : QFileInfo::exists(m_json->path(id)) ? "present"
                                                                 : "unknown";
    observation.bytesState = read.bytesAvailable                                ? "available"
                             : read.observedPresence && !*read.observedPresence ? "absent"
                                                                                : "unavailable";
    observation.bytes = read.bytes;
    observation.revision = read.revision;
    observation.observationOrder = ++m_observationOrder;
    if (command) {
        observation.requestId =
            command->requestId ? QString::number(command->requestId) : QString();
        observation.groupToken = command->groupToken;
        if (!command->groupToken.isEmpty())
            observation.attemptId =
                m_groups.value(command->groupToken).info.attempts.isEmpty()
                    ? QString()
                    : m_groups.value(command->groupToken).info.attempts.last().attemptId;
    }
    const auto reference = retainEvidence(m_options.evidenceDirectory, observation);
    if (!reference)
        return "evidence-retention";
    for (auto &state : m_sourceStates)
        if (state.sourceId == id)
            state.evidence.append(*reference);
    if (result)
        result->evidence.append(*reference);
    return {};
}
void ConfigWorker::acknowledgeModels(const QMap<ConfigPath, quint64> &generations) {
    Q_ASSERT(QThread::currentThread() == thread());
    for (auto it = generations.cbegin(); it != generations.cend(); ++it) {
        auto &versions = m_modelVersions[it.key()];
        auto end = versions.lowerBound(it.value());
        versions.erase(versions.begin(), end);
    }
}
void ConfigWorker::post(const ConfigWorkerPacket &input) {
    auto packet = input;
    const auto diagnostics = m_options.schema.diagnostics();
    if (packet.result)
        packet.result = diagnostics.sanitize(*packet.result);
    if (packet.sourceError)
        packet.sourceError = diagnostics.sanitize(*packet.sourceError);
    if (packet.migrationResult)
        packet.migrationResult = diagnostics.sanitize(*packet.migrationResult);
    if (m_holdReplies) {
        m_replies.enqueue(packet);
        return;
    }
    emit packetReady(packet);
}
void ConfigWorker::enqueue(const ConfigCommand &command) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (command.operation == "close" || command.operation == "retire")
        overlayCancelAll();
    m_queue.enqueue(command);
    drain();
}
void ConfigWorker::drain() {
    if (m_draining)
        return;
    m_draining = true;
    while (!m_busy && !m_holdInputs && !m_queue.isEmpty()) {
        qsizetype index = 0;
        while (index < m_queue.size() && overlayBlocked(m_queue[index]))
            ++index;
        if (index == m_queue.size())
            break;
        const auto command = m_queue.takeAt(index);
        try {
            process(command);
        } catch (const std::exception &) {
            fail("worker-exception");
            auto result = resultFor(command);
            result.status = "failed";
            result.error = {"worker-exception", {}, command.operation};
            post(publish(result));
        } catch (...) {
            fail("worker-exception");
            auto result = resultFor(command);
            result.status = "failed";
            result.error = {"worker-exception", {}, command.operation};
            post(publish(result));
        }
    }
    m_draining = false;
}
void ConfigWorker::coverInput(const ConfigCommand &command) {
    if (!command.input)
        return;
    const auto &input = *command.input;
    if (input.sequence > m_prefix)
        m_settledInputs.insert(input.sequence);
    while (m_settledInputs.remove(m_prefix + 1))
        ++m_prefix;
    for (auto it = input.pathGenerations.cbegin(); it != input.pathGenerations.cend(); ++it) {
        m_pathGenerations[it.key()] = it.value();
        m_pathInputs[it.key()] = input.sequence;
    }
}
void ConfigWorker::acceptIntent(const ConfigInput &input) {
    // This is the common authoritative fence for all accepted producers. A
    // worker-resolved profile group never fabricates a GUI path generation.
    QList<ConfigPath> paths = input.pathGenerations.keys();
    if (input.kind == ConfigInput::Kind::Group)
        for (const auto &leaf : m_options.schema.leaves())
            for (auto edit = input.edits.cbegin(); edit != input.edits.cend(); ++edit)
                if (startsWith(leaf, edit.key()) && !paths.contains(leaf))
                    paths.append(leaf);
    for (const auto &path : paths) {
        ++m_pathIntents[path];
        m_pathInputs[path] = qMax(m_pathInputs.value(path), input.sequence);
    }
}
void ConfigWorker::process(const ConfigCommand &command) {
    if (command.operation == "overlayLease") {
        overlayAcquire(command.overlay);
        return;
    }
    // Every settled input covers its sequence/generation, including terminal
    // refusal. Retained intent does not keep a rejected local value effective.
    if (!command.input || command.input->kind != ConfigInput::Kind::Group || m_failed || m_closed)
        coverInput(command);
    if (m_failed || m_closed || !m_layers->valid()) {
        auto result = resultFor(command);
        result.status = "failed";
        result.error = {m_closed ? "store-closed" : "worker-stopped", {}, command.operation};
        inheritFailures(result);
        post(publish(result));
        if (command.operation == "close")
            m_closed = true;
        return;
    }
    if (command.operation.startsWith("overlay") || command.operation == "openOverlay") {
        overlayCommand(command);
        return;
    }
    if (command.operation == "retainGroup" || command.operation == "saveGroup" ||
        command.operation == "retryGroup" || command.operation == "releaseGroup") {
        retainedGroupCommand(command);
        return;
    }
    if (!command.input) {
        if (command.operation == "updateRecordListPolicy") {
            recordListCommand(command);
            return;
        }
        if (command.operation == "exportProfile" || command.operation == "importProfile") {
            profile(command);
            return;
        }
        if (command.operation == "retire") {
            auto result = resultFor(command);
            result.status = "nothing_to_save";
            m_closed = true;
            post(publish(result));
            return;
        }
        if (command.operation == "reload") {
            reloadFiles(command);
            return;
        }
        beginSave(command);
        return;
    }
    const auto &input = *command.input;
    if (command.operation == "editRecordList") {
        recordListCommand(command);
        return;
    }
    if (input.kind == ConfigInput::Kind::Group) {
        processGroup(command);
        return;
    }
    auto result = resultFor(command);
    auto rejection = m_options.admission ? m_options.admission(input) : QString();
    if (input.kind != ConfigInput::Kind::Runtime && input.kind != ConfigInput::Kind::ClearRuntime &&
        rejection.isEmpty())
        rejection = quarantined(input.path);
    ConfigWriteResult write;
    const auto before = *m_layers;
    if (!rejection.isEmpty())
        write.errors = {m_options.schema.diagnostics().make(input.path, rejection)};
    else
        switch (input.kind) {
        case ConfigInput::Kind::User:
            write = m_layers->setUser(input.path, input.value);
            break;
        case ConfigInput::Kind::Initialize: {
            const auto route = m_layers->info(input.path).route;
            const auto source = m_layers->source(route.layerId, route.sourceId);
            if (!source || valueAt(source->raw, input.path.mid(route.prefix.size())))
                write.accepted = bool(source);
            else
                write = m_layers->setUser(input.path, input.value);
            break;
        }
        case ConfigInput::Kind::Runtime:
            write = m_layers->setRuntime(input.path, input.value);
            break;
        case ConfigInput::Kind::Reset:
            write = m_layers->reset(input.path, input.target);
            break;
        case ConfigInput::Kind::ClearRuntime:
            write = m_layers->clearRuntime(input.path);
            break;
        case ConfigInput::Kind::Group:
            break;
        }
    result.maskedBy = m_layers->info(input.path).maskedBy;
    result.status = write.accepted ? "accepted" : "failed";
    if (write.accepted)
        acceptIntent(input);
    if (!write.accepted) {
        const auto diagnostic = write.errors.isEmpty()
                                    ? m_options.schema.diagnostics().make(input.path, "admission")
                                    : write.errors.first();
        result.error = m_options.schema.diagnostics().error(diagnostic, {}, "write");
        m_failures.append(result);
    } else if (input.kind == ConfigInput::Kind::User || input.kind == ConfigInput::Kind::Reset ||
               input.kind == ConfigInput::Kind::Initialize) {
        recordPersistent(input, before);
        scheduleAutoSave();
    }
    post(publish(result));
}
void ConfigWorker::beginSave(const ConfigCommand &command) {
    m_autoTimer->stop();
    m_save = command;
    m_saveResult = resultFor(command);
    m_saveIntents.clear();
    m_saveBases.clear();
    m_saveRevisions.clear();
    m_saveBytes.clear();
    m_recoveryRead = false;
    m_recoveryChecked.clear();
    m_recoveryWhole.clear();
    m_unresolvedSave = false;
    if (!command.groupToken.isEmpty()) {
        describeGroup(m_saveResult, m_groups.value(command.groupToken).info);
        m_saveResult.coveredRevisions = m_groups.value(command.groupToken).confirmedRevisions;
    } else {
        m_saveResult.unresolvedGroups = unresolvedGroups(command.prefix);
        m_saveResult.excludedGroups = m_saveResult.unresolvedGroups;
        m_unresolvedSave =
            command.operation != "autosave" && !m_saveResult.unresolvedGroups.isEmpty();
    }
    if (command.input && command.input->kind == ConfigInput::Kind::Group) {
        QSet<QString> masks;
        for (auto it = command.input->edits.cbegin(); it != command.input->edits.cend(); ++it) {
            const auto mask = m_layers->info(it.key()).maskedBy;
            m_saveResult.masks[it.key()] = mask;
            if (!mask.isEmpty())
                masks.insert(mask);
        }
        if (masks.size() == 1)
            m_saveResult.maskedBy = *masks.cbegin();
    }
    m_saveRetainedFailure = command.groupToken.isEmpty() && inheritFailures(m_saveResult);
    if (m_saveRetainedFailure) {
        finishSave();
        return; // Preserve the original outcome; never silently retry or recapture.
    }
    for (const auto &id : m_dirty.keys()) {
        const auto intents = eligibleIntents(id, command);
        if (intents.isEmpty())
            continue;
        m_saveIntents[id] = intents;
        m_saveBases[id] = command.groupToken.isEmpty()
                              ? m_baselines.value(id)
                              : m_groups.value(command.groupToken).bases.value(id);
        m_saveRevisions[id] = command.groupToken.isEmpty()
                                  ? m_pendingRevisions.value(id, m_revisions.value(id))
                                  : m_groups.value(command.groupToken).revisions.value(id);
        m_saveBytes[id] = command.groupToken.isEmpty()
                              ? m_pendingBytes.value(id, m_sourceBytes.value(id))
                              : m_groups.value(command.groupToken).bytes.value(id);
        if (command.groupToken.isEmpty())
            for (const auto &path : m_options.schema.leaves()) {
                quint64 through = 0;
                for (const auto &intent : intents)
                    if (!reserved(path) &&
                        (startsWith(path, intent.path) || startsWith(intent.path, path)))
                        through = qMax(through, intent.input);
                if (through)
                    m_saveResult.coverage.append({path, id, through, m_pathIntents.value(path)});
            }
    }
    if (m_saveIntents.isEmpty() || m_routes.isEmpty()) {
        m_saveResult.status = "nothing_to_save";
        finishSave();
        return;
    }
    m_busy = true;
    m_saveSources = m_saveIntents.keys();
    continueSave();
}
void ConfigWorker::continueSave() {
    if (m_activeBackend) {
        auto outcome = m_backend->take(m_activeBackend);
        if (!outcome)
            return;
        m_activeBackend = 0;
        if (m_recoveryRead) {
            m_recoveryRead = false;
            if (recoverOwnPublication(m_activeSource, *outcome))
                m_saveSources.prepend(m_activeSource);
            continueSave();
            return;
        }
        if (outcome->success && outcome->bytes != m_activeBytes) {
            outcome->success = false;
            outcome->publicationState = ConfigBackendResult::PublicationState::Uncertain;
            outcome->error = {"readback-mismatch", m_activeSource, "readback"};
        }
        recordPublication(m_saveResult, m_activeSource, *outcome);
        if (outcome->success) {
            confirmed(m_activeSource, decodeMemoryValue(outcome->bytes), *outcome);
        } else {
            if (!m_save.groupToken.isEmpty() && outcome->publicationWitness) {
                auto &capture = m_groups[m_save.groupToken];
                capture.uncertain[m_activeSource] = *outcome->publicationWitness;
                capture.uncertainBytes[m_activeSource] = m_activeBytes;
            }
        }
    }
    if (m_saveSources.isEmpty()) {
        finishSave();
        return;
    }
    m_activeSource = m_saveSources.takeFirst();
    bool uncertain = false;
    for (const auto &capture : m_groups)
        uncertain |= capture.uncertain.contains(m_activeSource);
    if (uncertain && !m_recoveryChecked.contains(m_activeSource)) {
        m_recoveryChecked.insert(m_activeSource);
        if (!m_files.contains(m_activeSource)) {
            m_recoveryRead = true;
            m_activeBackend =
                m_backend->submit({ConfigBackendRequest::Operation::Read, m_activeSource, {}, {}});
            continueSave();
            return;
        }
        const auto id =
            m_json->submit({ConfigBackendRequest::Operation::Read, m_activeSource, {}, {}});
        if (!recoverOwnPublication(m_activeSource, *m_json->take(id))) {
            continueSave();
            return;
        }
    }
    if (m_files.contains(m_activeSource)) {
        ConfigBackendResult original;
        original.success = true;
        original.revision = m_saveRevisions.value(m_activeSource);
        original.bytes = m_saveBytes.value(m_activeSource);
        original.observedPresence = original.revision.exists;
        original.bytesAvailable = original.revision.exists;
        auto retentionError = retainObservation(m_activeSource, original, "preflight", "original",
                                                m_save, &m_saveResult);
        if (!retentionError.isEmpty()) {
            m_saveResult.status = "failed";
            m_saveResult.error = {retentionError, m_activeSource, "preflight"};
            continueSave();
            return;
        }
        QList<ConfigFileEdit> edits;
        for (const auto &intent : m_saveIntents.value(m_activeSource))
            edits.append(intent.edit);
        if (m_recoveryWhole.contains(m_activeSource)) {
            auto raw = m_saveBases.value(m_activeSource);
            for (const auto &edit : edits)
                applyFileEdit(raw, edit);
            edits = {{{}, raw, false}};
        }
        auto transaction =
            m_json->reconcile(m_activeSource, m_saveBases.value(m_activeSource), edits,
                              ConfigSourcePreimage{m_saveRevisions.value(m_activeSource),
                                                   m_saveBytes.value(m_activeSource)});
        const auto &outcome = transaction.outcome;
        for (const auto &observation : transaction.observations) {
            const auto error =
                retainObservation(m_activeSource, observation.result, observation.phase,
                                  observation.role, m_save, &m_saveResult);
            if (!error.isEmpty())
                retentionError = error;
        }
        recordPublication(m_saveResult, m_activeSource, outcome, transaction.published);
        if (outcome.success) {
            confirmed(m_activeSource, transaction.raw, outcome);
            const auto retention = observeSource(m_activeSource, outcome, {}, false);
            if (!retention.isEmpty()) {
                m_saveResult.status = "failed";
                m_saveResult.error = {retention, m_activeSource, "readback"};
            }
            m_acceptOrigin = "own_save";
            m_acceptCause = "save";
        } else {
            if (!m_save.groupToken.isEmpty() && outcome.publicationWitness) {
                auto &capture = m_groups[m_save.groupToken];
                capture.uncertain[m_activeSource] = *outcome.publicationWitness;
                capture.uncertainBytes[m_activeSource] = transaction.candidateBytes;
            }
        }
        if (!retentionError.isEmpty()) {
            m_saveResult.status = "failed";
            m_saveResult.error = {retentionError, m_activeSource, "readback"};
        }
        armWatches();
        continueSave();
        return;
    }
    m_activeRaw = m_saveBases.value(m_activeSource);
    for (const auto &intent : m_saveIntents.value(m_activeSource))
        applyFileEdit(m_activeRaw, intent.edit);
    m_activeBytes = encodeMemoryValue(m_activeRaw);
    m_activeBackend = m_backend->submit({ConfigBackendRequest::Operation::Publish, m_activeSource,
                                         m_activeBytes, m_saveRevisions.value(m_activeSource)});
    continueSave(); // Immediate memory completion or deterministic delayed operation.
}
void ConfigWorker::recordPublication(ConfigResult &result, const QString &source,
                                     const ConfigBackendResult &outcome, bool published) {
    const QString state = outcome.publicationState == ConfigBackendResult::PublicationState::Known
                              ? "known"
                              : "uncertain";
    if (published || !outcome.success)
        result.publications.append(
            {source, outcome.success, outcome.revision, state, outcome.error});
    if (outcome.success)
        result.coveredRevisions[source] = outcome.revision;
    else {
        result.status = "failed";
        result.error = outcome.error;
        if (state == "uncertain")
            result.publicationState = state;
    }
}
void ConfigWorker::finalizePublicationState(ConfigResult &result) {
    if (result.status == "failed" && !result.coveredRevisions.isEmpty())
        result.publicationState = "partial";
}
void ConfigWorker::finishSave() {
    if (m_saveResult.status.isEmpty())
        m_saveResult.status = m_saveResult.publications.isEmpty() ? "nothing_to_save" : "saved";
    if (m_saveResult.status != "failed" && m_save.groupToken.isEmpty() &&
        m_saveResult.excludedGroups.isEmpty())
        m_saveResult.coveredRevisions = m_revisions;
    const bool refusal = m_unresolvedSave && m_saveResult.status != "failed";
    if (refusal) {
        m_saveResult.status = "failed";
        m_saveResult.error = {"unresolved-group", {}, m_save.operation};
    }
    if (m_saveResult.status == "failed") {
        finalizePublicationState(m_saveResult);
        if (!m_saveRetainedFailure && !refusal && m_save.groupToken.isEmpty())
            m_failures.append(m_saveResult);
    }
    if (!m_save.groupToken.isEmpty()) {
        auto &info = m_groups[m_save.groupToken].info;
        auto &attempt = info.attempts.last();
        attempt.status = m_saveResult.status;
        attempt.publicationState = m_saveResult.publicationState;
        attempt.error = m_saveResult.error;
        info.state = m_saveResult.status == "failed" ? "stopped" : "succeeded";
        ++m_groups[m_save.groupToken].generation;
        describeGroup(m_saveResult, info);
        m_groups[m_save.groupToken].receipts.append(m_saveResult);
        m_saveResult.unresolvedGroups = unresolvedGroups(m_save.prefix);
        if (info.state == "succeeded")
            scheduleAutoSave();
    }
    const bool closing = m_save.operation == "close";
    if (!m_save.editorId.isEmpty()) {
        const auto command = m_save;
        auto receipt = m_saveResult;
        receipt.editorId = command.editorId;
        receipt.refreshState = "not_refreshed";
        post(publish());
        m_busy = false;
        if (receipt.status == "failed") {
            post(publish(receipt));
        } else {
            m_overlayReceipts[command.editorId] = receipt;
            auto refresh = command;
            refresh.operation = "overlayRefresh";
            refresh.input.reset();
            overlayCommand(refresh);
        }
        drain();
        return;
    }
    post(publish(m_saveResult));
    m_busy = false;
    if (closing)
        m_closed = true;
    else
        drain();
}
void ConfigWorker::holdInputs(bool hold) {
    m_holdInputs = hold;
    if (!hold)
        drain();
}
void ConfigWorker::releaseInputs(int count) {
    for (int i = 0; i < count && !m_queue.isEmpty() && !m_busy; ++i) {
        const auto command = m_queue.dequeue();
        process(command);
    }
}
void ConfigWorker::holdReplies(bool hold) {
    m_holdReplies = hold;
    if (!hold)
        releaseReplies(m_replies.size());
}
void ConfigWorker::releaseReplies(int count) {
    const bool held = m_holdReplies;
    m_holdReplies = false;
    while (count-- > 0 && !m_replies.isEmpty())
        post(m_replies.dequeue());
    m_holdReplies = held;
}
void ConfigWorker::releaseBackend() {
    if (m_failed || !m_activeBackend)
        return;
    for (auto id : m_backend->pending())
        m_backend->release(id);
    continueSave();
}
void ConfigWorker::scheduleBackend(ConfigMemoryBackend::Schedule schedule) {
    m_backend->schedule(schedule);
}
void ConfigWorker::fail(const QString &code) {
    if (m_failed)
        return;
    m_failed = true;
    overlayCancelAll();
    ConfigResult failure;
    failure.storeId = m_options.storeId;
    failure.status = "failed";
    failure.inputPrefix = m_prefix;
    failure.error = {code, {}, "worker"};
    m_failures.append(failure);
    auto packet = publish();
    packet.terminal = true;
    packet.error = code;
    post(packet);
    if (m_busy) {
        m_saveResult.status = "failed";
        m_saveResult.error = failure.error;
        if (m_activeBackend)
            m_saveResult.publicationState = "uncertain";
        m_activeBackend = 0;
        m_saveSources.clear();
        finishSave();
    }
    drain();
}
void ConfigWorker::abandon() {
    // Only the process-owned worker owner retires a settled terminal command.
    thread()->quit();
}
void ConfigWorker::scheduleAutoSave() {
    if (m_options.autoSave && !m_closed && !m_failed)
        m_autoTimer->start(qMax(0, m_options.autoSaveInterval));
}
void ConfigWorker::armWatches() {
    QSet<QString> wanted;
    for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
        if (it->watchMode != ConfigWatchMode::Live)
            continue;
        for (const auto &path : m_json->watchPaths(it.key())) {
            if (QFileInfo::exists(path))
                wanted.insert(path);
            auto parent = QFileInfo(path).absolutePath();
            while (!QFileInfo::exists(parent) && parent != "/")
                parent = QFileInfo(parent).absolutePath();
            wanted.insert(parent);
        }
    }
    const auto watched = m_watcher->files() + m_watcher->directories();
    for (const auto &path : watched)
        if (!wanted.contains(path))
            m_watcher->removePath(path);
    for (const auto &path : wanted)
        if (!watched.contains(path))
            m_watcher->addPath(path);
}
bool ConfigWorker::reloadFiles(const std::optional<ConfigCommand> &command) {
    std::optional<ConfigResult> result;
    if (command) {
        result = resultFor(*command);
        result->status = "reloaded";
    }
    QList<ConfigError> errors;
    QMap<QString, QList<ConfigSource>> replacements;
    QMap<QString, QList<ConfigSource>> diskReplacements;
    bool changed = false;
    for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
        const auto sourcePrefix = m_fileRoutes.value(it.key()).prefix;
        if (!m_reloadSelection.isEmpty()) {
            bool selected = false;
            for (const auto &path : m_reloadSelection)
                selected |= startsWith(path, sourcePrefix) || startsWith(sourcePrefix, path);
            if (!selected)
                continue;
        } else if (!command) {
            bool leased = false;
            for (const auto &paths : m_overlayLeases)
                for (const auto &path : paths)
                    leased |= startsWith(path, sourcePrefix) || startsWith(sourcePrefix, path);
            if (leased)
                continue;
        }
        if (!command && (it->watchMode != ConfigWatchMode::Live || !m_json->changed(it.key())))
            continue;
        auto id = m_json->submit({ConfigBackendRequest::Operation::Read, it.key(), {}, {}});
        const auto read = *m_json->take(id);
        const auto route = m_fileRoutes.value(it.key());
        ConfigJsonCodec codec(m_options.schema.diagnostics().at(route.prefix));
        auto decoded =
            read.revision.exists ? codec.decode(read.bytes, it.key()) : ConfigDecodeResult{};
        ConfigError error = !read.success ? read.error : decoded.error;
        const auto retention = observeSource(it.key(), read, error, false);
        const auto retained =
            retainObservation(it.key(), read, "read", read.success ? "foreign" : "late", command,
                              result ? &*result : nullptr);
        if (!retention.isEmpty() || !retained.isEmpty())
            error = {"evidence-retention", it.key(), "read"};
        if (!read.revision.exists && !it->optional && error.empty())
            error = {"required-file-missing", it.key(), "read"};
        if (!error.empty()) {
            error.file = m_json->path(it.key());
            errors.append(error);
            continue; // Parse/IO failures never destroy last-good layer state.
        }
        const auto current = m_layers->source(route.layerId, route.sourceId);
        if (current->available && m_layers->diagnostics(route.layerId, route.sourceId).isEmpty() &&
            m_revisions.contains(it.key()) && read.revision == m_revisions.value(it.key()) &&
            read.bytes == m_sourceBytes.value(it.key())) {
            // The accepted disk observation is unchanged. Re-admitting its
            // raw-plus-intent replay would replace pending contribution fences
            // with disk provenance and invent a new displayed generation.
            // Source state/evidence above still describe this successful read.
            continue;
        }
        auto source = *current;
        source.raw = decoded.raw;
        source.bytes = read.bytes;
        source.revision = read.revision.token;
        source.exists = read.revision.exists;
        source.diskExists = read.revision.exists;
        source.available = true;
        source.localIntent = false;
        if (m_options.schema.version() > 0 && source.exists) {
            const auto version = ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)
                                     .validate(source.raw.value("_configqmlVersion", 0));
            if (m_dirty.contains(it.key()) &&
                (!version.ok() || version.value.toInt() != m_options.schema.version())) {
                errors.append(
                    {"migration-pending-intent", it.key(), "migration", m_json->path(it.key())});
                continue;
            }
            // Reuse the same transform/backup/publication owner on every load,
            // including explicit and watched reloads. No facade migration path.
            m_options.layers = {{route.layerId,
                                 ConfigLayerKind::File,
                                 ConfigAdmissionPolicy::FieldFallback,
                                 {source},
                                 m_routes.contains(it.key())}};
            const bool migrated = migrateSources();
            if (migrated)
                source = m_options.layers.first().sources.first();
            else
                errors.append(
                    {m_initializationError, it.key(), "migration", m_json->path(it.key())});
            m_initializationError.clear();
            m_options.layers.clear();
            if (!migrated)
                continue;
        }
        const ConfigRevision admittedRevision{source.exists, source.revision};
        m_json->confirm(it.key(), admittedRevision);
        if (!m_dirty.contains(it.key()))
            m_baselines[it.key()] = source.raw;
        diskReplacements[route.layerId].append(source);
        for (const auto &edit : pendingEdits(it.key()))
            applyFileEdit(source.raw, edit);
        if (!m_intents.value(it.key()).isEmpty())
            source.exists = true;
        source.localIntent = !m_intents.value(it.key()).isEmpty();
        replacements[route.layerId].append(source);
        m_revisions[it.key()] = admittedRevision;
        m_sourceBytes[it.key()] = source.bytes;
        changed = true;
    }
    m_diskLayers->replaceAllSources(diskReplacements);
    if (!replacements.isEmpty()) {
        const auto admission = m_layers->replaceAllSources(replacements);
        if (!admission.accepted && !admission.errors.isEmpty()) {
            const auto diagnostic = admission.errors.first();
            QString id;
            for (const auto &sources : replacements)
                for (const auto &source : sources)
                    if (startsWith(diagnostic.path, source.prefix))
                        id = source.id;
            errors.append(m_options.schema.diagnostics().error(diagnostic, id, "admission",
                                                               m_json->path(id)));
        }
    }
    armWatches();
    if (result && !errors.isEmpty()) {
        result->status = "failed";
        result->error = errors.first();
    }
    if (changed || result || !errors.isEmpty()) {
        if (changed) {
            m_acceptOrigin = "external";
            m_acceptCause = command ? "reload" : "watch";
        }
        post(publish(result));
    }
    for (const auto &error : errors) {
        ConfigWorkerPacket packet;
        packet.sourceError = error;
        post(packet);
    }
    return errors.isEmpty();
}
} // namespace configqml
