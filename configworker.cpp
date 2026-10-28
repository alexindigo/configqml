// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "confignumber.h"
#include <QDataStream>
#include <QIODevice>
#include <QThread>
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
    m_layers = std::make_unique<ConfigLayers>(m_options.schema, m_options.layers);
    m_backend = std::make_unique<ConfigMemoryBackend>();
    for (const auto &layer : m_options.layers) {
        if (!layer.writable)
            continue;
        for (const auto &source : layer.sources) {
            // Backends are per logical store; source identity is unique within its user layer.
            m_routes[source.id] = {layer.id, source.id, source.prefix};
            m_backend->seed(source.id, encodeMemoryValue(source.raw), source.exists);
            auto id = m_backend->submit({ConfigBackendRequest::Operation::Read, source.id, {}, {}});
            m_revisions[source.id] = m_backend->take(id)->revision;
        }
    }
    m_options.layers.clear();
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
        state.pathGeneration = m_pathGenerations.value(path);
        state.publicationGeneration = packet.snapshot.generation;
        const auto before = m_previous.information.value(path);
        if (initial || next != m_previous.values.value(path) ||
            state.pathGeneration != before.pathGeneration ||
            state.provenance.layerId != before.provenance.layerId ||
            state.provenance.maskedBy != before.provenance.maskedBy) {
            packet.snapshot.values[path] = next;
            packet.snapshot.information[path] = state;
        }
        m_previous.values[path] = next;
        m_previous.information[path] = state;
    }
    m_previous.generation = m_generation;
    m_previous.inputPrefix = m_prefix;
    packet.admitted = std::make_shared<const ConfigSnapshot>(m_previous);
    return packet;
}
void ConfigWorker::post(const ConfigWorkerPacket &packet) {
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
void ConfigWorker::process(const ConfigCommand &command) {
    // Every settled input covers its sequence/generation, including terminal
    // refusal. Retained intent does not keep a rejected local value effective.
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
        beginSave(command);
        return;
    }
    const auto &input = *command.input;
    auto result = resultFor(command);
    const auto rejection = m_options.admission ? m_options.admission(input) : QString();
    ConfigWriteResult write;
    if (!rejection.isEmpty())
        write.errors = {{input.path, rejection}};
    else
        switch (input.kind) {
        case ConfigInput::Kind::User:
            write = m_layers->setUser(input.path, input.value);
            break;
        case ConfigInput::Kind::Runtime:
            write = m_layers->setRuntime(input.path, input.value);
            break;
        case ConfigInput::Kind::Reset:
            write = m_layers->reset(input.path, input.target);
            break;
        case ConfigInput::Kind::ClearRuntime:
            write = m_layers->clearRuntime(input.path);
            break;
        }
    result.maskedBy = m_layers->info(input.path).maskedBy;
    result.status = write.accepted ? "accepted" : "failed";
    if (!write.accepted) {
        result.error = {
            write.errors.isEmpty() ? QString("admission") : write.errors.first().code, {}, "write"};
        m_failures.append(result);
    } else if (input.kind == ConfigInput::Kind::User || input.kind == ConfigInput::Kind::Reset) {
        for (auto it = m_routes.cbegin(); it != m_routes.cend(); ++it)
            if (startsWith(input.path, it->prefix) || startsWith(it->prefix, input.path))
                m_dirty[it.key()] = input.sequence;
    }
    post(publish(result));
}
void ConfigWorker::beginSave(const ConfigCommand &command) {
    m_save = command;
    m_saveResult = resultFor(command);
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
    m_activeBytes = encodeMemoryValue(m_layers->source(route.layerId, route.sourceId)->raw);
    m_activeBackend = m_backend->submit({ConfigBackendRequest::Operation::Publish, m_activeSource,
                                         m_activeBytes, m_revisions.value(m_activeSource)});
    continueSave(); // Immediate memory completion or deterministic delayed operation.
}
void ConfigWorker::finishSave() {
    if (m_saveResult.status.isEmpty())
        m_saveResult.status = "saved";
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
    // Destructor never waits on a worker or starts an implicit save. Explicit
    // close is the asynchronous persistence/lifetime fence for short-lived writers.
    thread()->quit();
}
} // namespace configqml
