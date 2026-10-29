// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
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
    m_backend = std::make_unique<ConfigMemoryBackend>();
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
        bool found = false;
        for (auto &layer : m_options.layers)
            if (layer.id == file.layerId && layer.kind == ConfigLayerKind::File)
                for (auto &source : layer.sources)
                    if (source.id == file.sourceId) {
                        found = true;
                        if (sourceIds.value(file.sourceId) != 1 ||
                            m_files.contains(file.sourceId) ||
                            paths.contains(m_json->path(file.sourceId)))
                            m_initializationError = "duplicate-file-identity";
                        paths.insert(m_json->path(file.sourceId));
                        m_files[file.sourceId] = file;
                        m_fileRoutes[file.sourceId] = {layer.id, source.id, source.prefix};
                        source.raw.clear();
                        source.exists = false;
                        source.json = true;
                        if (file.watchMode == ConfigWatchMode::Off)
                            continue;
                        const auto id = m_json->submit(
                            {ConfigBackendRequest::Operation::Read, file.sourceId, {}, {}});
                        auto read = *m_json->take(id);
                        ConfigJsonCodec codec(m_options.schema.diagnostics().at(source.prefix));
                        const auto decoded = read.revision.exists
                                                 ? codec.decode(read.bytes, file.sourceId)
                                                 : ConfigDecodeResult{};
                        if (!read.success || !decoded.ok() ||
                            (!read.revision.exists && !file.optional)) {
                            m_initializationError = !read.success   ? read.error.code
                                                    : !decoded.ok() ? decoded.error.code
                                                                    : "required-file-missing";
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
                        m_baselines[file.sourceId] = source.raw;
                        m_revisions[file.sourceId] = read.revision;
                        m_json->confirm(file.sourceId, read.revision);
                    }
        if (!found)
            m_initializationError = "unknown-file-route";
    }
    migrateSources();
    m_layers = std::make_unique<ConfigLayers>(m_options.schema, m_options.layers);
    if (m_initializationError.isEmpty() && !m_layers->rejectedDiagnostics().isEmpty())
        m_initializationError = "strict-file-admission";
    for (const auto &layer : m_options.layers) {
        if (!layer.writable)
            continue;
        for (const auto &source : layer.sources) {
            // Backends are per logical store; source identity is unique within its user layer.
            m_routes[source.id] = {layer.id, source.id, source.prefix};
            if (m_files.contains(source.id))
                continue;
            m_backend->seed(source.id, encodeMemoryValue(source.raw), source.exists);
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
    packet.initial = initial;
    packet.result = result;
    packet.snapshot.generation = ++m_generation;
    packet.snapshot.inputPrefix = m_prefix;
    const auto effective = m_layers->effective(); // One full-tree owner, never the GUI.
    for (const auto &path : m_options.schema.leaves()) {
        const auto next = valueAt(effective, path).value_or(QVariant());
        ConfigPathState state;
        state.provenance = m_layers->info(path);
        state.workerCovered = m_prefix;
        state.latestInput = m_pathInputs.value(path);
        state.intentRevision = m_pathIntents.value(path);
        QVariantList content{next, QString::number(state.intentRevision)};
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
        }
        m_previous.values[path] = next;
        m_previous.information[path] = state;
    }
    m_previous.generation = m_generation;
    m_previous.inputPrefix = m_prefix;
    packet.admitted = std::make_shared<const ConfigSnapshot>(m_previous);
    return packet;
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
    m_queue.enqueue(command);
    drain();
}
void ConfigWorker::drain() {
    if (m_draining)
        return;
    m_draining = true;
    while (!m_busy && !m_holdInputs && !m_queue.isEmpty()) {
        const auto command = m_queue.dequeue();
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
    m_prefix = input.sequence;
    for (auto it = input.pathGenerations.cbegin(); it != input.pathGenerations.cend(); ++it) {
        m_pathGenerations[it.key()] = it.value();
        m_pathInputs[it.key()] = input.sequence;
    }
}
void ConfigWorker::acceptIntent(const ConfigInput &input) {
    // This is the common authoritative fence for all accepted producers. A
    // worker-resolved profile group never fabricates a GUI path generation.
    const auto paths =
        input.kind == ConfigInput::Kind::Group ? input.edits.keys() : input.pathGenerations.keys();
    for (const auto &path : paths) {
        ++m_pathIntents[path];
        m_pathInputs[path] = qMax(m_pathInputs.value(path), input.sequence);
    }
}
void ConfigWorker::process(const ConfigCommand &command) {
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
    if (!command.input) {
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
    if (input.kind == ConfigInput::Kind::Group) {
        processGroup(command);
        return;
    }
    auto result = resultFor(command);
    const auto rejection = m_options.admission ? m_options.admission(input) : QString();
    ConfigWriteResult write;
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
        for (auto it = m_routes.cbegin(); it != m_routes.cend(); ++it)
            if (startsWith(input.path, it->prefix) || startsWith(it->prefix, input.path)) {
                m_dirty[it.key()] = input.sequence;
                if (m_files.contains(it.key())) {
                    const auto relative = input.path.mid(it->prefix.size());
                    m_edits[it.key()].append({relative,
                                              input.kind == ConfigInput::Kind::Reset
                                                  ? std::optional<QVariant>()
                                                  : input.value,
                                              input.kind == ConfigInput::Kind::Initialize});
                }
            }
        scheduleAutoSave();
    }
    post(publish(result));
}
void ConfigWorker::beginSave(const ConfigCommand &command) {
    m_autoTimer->stop();
    m_save = command;
    m_saveResult = resultFor(command);
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
    m_saveRetainedFailure = inheritFailures(m_saveResult);
    if (m_saveRetainedFailure) {
        finishSave();
        return; // Preserve the original outcome; never silently retry or recapture.
    }
    if (m_dirty.isEmpty() || m_routes.isEmpty()) {
        m_saveResult.status = "nothing_to_save";
        finishSave();
        return;
    }
    m_busy = true;
    m_saveSources = m_dirty.keys();
    continueSave();
}
void ConfigWorker::continueSave() {
    if (m_activeBackend) {
        auto outcome = m_backend->take(m_activeBackend);
        if (!outcome)
            return;
        m_activeBackend = 0;
        if (outcome->success && outcome->bytes != m_activeBytes) {
            outcome->success = false;
            outcome->publicationState = ConfigBackendResult::PublicationState::Uncertain;
            outcome->error = {"readback-mismatch", m_activeSource, "readback"};
        }
        const auto state = outcome->publicationState == ConfigBackendResult::PublicationState::Known
                               ? "known"
                               : "uncertain";
        m_saveResult.publications.append(
            {m_activeSource, outcome->success, outcome->revision, state, outcome->error});
        if (outcome->success) {
            m_revisions[m_activeSource] = outcome->revision;
            m_saveResult.coveredRevisions[m_activeSource] = outcome->revision;
            m_dirty.remove(m_activeSource);
        } else {
            m_saveResult.status = "failed";
            m_saveResult.error = outcome->error;
            if (QString(state) == "uncertain")
                m_saveResult.publicationState = "uncertain";
        }
    }
    if (m_saveSources.isEmpty()) {
        finishSave();
        return;
    }
    m_activeSource = m_saveSources.takeFirst();
    const auto route = m_routes.value(m_activeSource);
    if (m_files.contains(m_activeSource)) {
        auto transaction = m_json->reconcile(m_activeSource, m_baselines.value(m_activeSource),
                                             m_edits.value(m_activeSource));
        const auto &outcome = transaction.outcome;
        const QString state =
            outcome.publicationState == ConfigBackendResult::PublicationState::Known ? "known"
                                                                                     : "uncertain";
        if (transaction.published || !outcome.success)
            m_saveResult.publications.append(
                {m_activeSource, outcome.success, outcome.revision, state, outcome.error});
        if (outcome.success) {
            auto source = *m_layers->source(route.layerId, route.sourceId);
            source.raw = transaction.raw;
            source.bytes = outcome.bytes;
            source.revision = outcome.revision.token;
            source.exists = outcome.revision.exists;
            m_layers->replaceSource(route.layerId, source);
            m_baselines[m_activeSource] = transaction.raw;
            m_revisions[m_activeSource] = outcome.revision;
            m_saveResult.coveredRevisions[m_activeSource] = outcome.revision;
            m_dirty.remove(m_activeSource);
            m_edits.remove(m_activeSource);
        } else {
            m_saveResult.status = "failed";
            m_saveResult.error = outcome.error;
            if (state == "uncertain")
                m_saveResult.publicationState = state;
        }
        armWatches();
        continueSave();
        return;
    }
    m_activeBytes = encodeMemoryValue(m_layers->source(route.layerId, route.sourceId)->raw);
    m_activeBackend = m_backend->submit({ConfigBackendRequest::Operation::Publish, m_activeSource,
                                         m_activeBytes, m_revisions.value(m_activeSource)});
    continueSave(); // Immediate memory completion or deterministic delayed operation.
}
void ConfigWorker::finishSave() {
    if (m_saveResult.status.isEmpty())
        m_saveResult.status = m_saveResult.publications.isEmpty() ? "nothing_to_save" : "saved";
    if (m_saveResult.status != "failed")
        m_saveResult.coveredRevisions = m_revisions;
    if (m_saveResult.status == "failed") {
        bool success = false;
        for (const auto &p : m_saveResult.publications)
            success |= p.success;
        if (success)
            m_saveResult.publicationState = "partial";
        if (!m_saveRetainedFailure)
            m_failures.append(m_saveResult);
    }
    const bool closing = m_save.operation == "close";
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
    bool changed = false;
    for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
        if (!command && (it->watchMode != ConfigWatchMode::Live || !m_json->changed(it.key())))
            continue;
        auto id = m_json->submit({ConfigBackendRequest::Operation::Read, it.key(), {}, {}});
        const auto read = *m_json->take(id);
        const auto route = m_fileRoutes.value(it.key());
        ConfigJsonCodec codec(m_options.schema.diagnostics().at(route.prefix));
        auto decoded =
            read.revision.exists ? codec.decode(read.bytes, it.key()) : ConfigDecodeResult{};
        ConfigError error = !read.success ? read.error : decoded.error;
        if (!read.revision.exists && !it->optional && error.empty())
            error = {"required-file-missing", it.key(), "read"};
        if (!error.empty()) {
            error.file = m_json->path(it.key());
            errors.append(error);
            continue; // Parse/IO failures never destroy last-good layer state.
        }
        auto source = *m_layers->source(route.layerId, route.sourceId);
        source.raw = decoded.raw;
        source.bytes = read.bytes;
        source.revision = read.revision.token;
        source.exists = read.revision.exists;
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
        for (const auto &edit : m_edits.value(it.key())) {
            if (edit.ifMissing && valueAt(source.raw, edit.path))
                continue;
            if (edit.value)
                putValue(source.raw, edit.path, *edit.value);
            else if (edit.path.isEmpty())
                source.raw.clear();
            else
                removeValue(source.raw, edit.path);
        }
        if (!m_edits.value(it.key()).isEmpty())
            source.exists = true;
        replacements[route.layerId].append(source);
        m_revisions[it.key()] = admittedRevision;
        changed = true;
    }
    for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) {
        const auto admission = m_layers->replaceSources(it.key(), it.value());
        if (!admission.accepted && !admission.errors.isEmpty()) {
            const auto diagnostic = admission.errors.first();
            QString id;
            for (const auto &source : it.value())
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
    if (changed || result)
        post(publish(result));
    for (const auto &error : errors) {
        ConfigWorkerPacket packet;
        packet.sourceError = error;
        post(packet);
    }
    return errors.isEmpty();
}
} // namespace configqml
