// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configstore.h"
#include "configworker_p.h"
#include "configshutdown_p.h"
#include "configconsumer_p.h"
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
    auto owned = options;
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
            source.prefix = ownPath(source.prefix);
            const auto raw = ownValue(source.raw);
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
ConfigPathState ConfigStore::info(const ConfigPath &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_cache.information.value(path);
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
ConfigRequestHandle ConfigStore::write(ConfigInput::Kind kind, const ConfigPath &path,
                                       const QVariant &value, const QString &target,
                                       QObject *context, Callback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    ConfigRequestHandle handle;
    ConfigConsumerGuard boundary(this);
    auto checked = (kind == ConfigInput::Kind::User || kind == ConfigInput::Kind::Runtime ||
                    kind == ConfigInput::Kind::Initialize)
                       ? m_schema.validate(path, value)
                       : ConfigValueResult{};
    if (!m_accepting || !m_owner->accepting() || m_status != Status::Ready)
        checked.errors = {
            {path, m_accepting && m_owner->accepting() ? "not-ready" : "store-closed"}};
    if (!m_error.isEmpty())
        checked.errors = {{path, m_error}};
    if ((kind == ConfigInput::Kind::Reset || kind == ConfigInput::Kind::ClearRuntime) &&
        !m_schema.descriptor(path))
        checked.errors = {{path, "undeclared-path"}};
    if (checked.ok() &&
        (kind == ConfigInput::Kind::User || kind == ConfigInput::Kind::Initialize) &&
        !canWriteUser(path))
        checked.errors = {{path, "read-only-path"}};
    if (checked.ok() && kind == ConfigInput::Kind::Reset) {
        const auto route = m_userRoutes.lowerBound(path);
        const bool coversSource = route != m_userRoutes.cend() && startsWith(route.key(), path);
        if (target != m_userLayerId || (!canWriteUser(path) && !coversSource))
            checked.errors = {{path, "read-only-path"}};
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
        result.error = {checked.errors.first().code, {}, "ingress"};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
        boundary.invoke([&] { emit inputRejected(path, result.error.code); });
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
        handle.errors = {{path, "store-closed"}};
        ConfigResult result;
        result.requestId = handle.requestId;
        result.storeId = m_storeId;
        result.operation = "write";
        result.status = "failed";
        result.inputPrefix = m_sequence;
        result.error = {"store-closed", {}, "ingress"};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
        boundary.invoke([&] { emit inputRejected(path, result.error.code); });
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
    }
    // Owned input, sequence, queueing and cache installation all precede consumer reentry.
    if (changed)
        boundary.invoke([&] { emit valuesChanged({path}); });
    return handle;
}
ConfigRequestHandle ConfigStore::setUser(const ConfigPath &p, const QVariant &v, QObject *c,
                                         Callback f) {
    return write(ConfigInput::Kind::User, p, v, {}, c, std::move(f));
}
ConfigRequestHandle ConfigStore::initializeIfMissing(const ConfigPath &p, const QVariant &v,
                                                     QObject *c, Callback f) {
    return write(ConfigInput::Kind::Initialize, p, v, {}, c, std::move(f));
}
ConfigRequestHandle ConfigStore::setRuntime(const ConfigPath &p, const QVariant &v, QObject *c,
                                            Callback f) {
    return write(ConfigInput::Kind::Runtime, p, v, {}, c, std::move(f));
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
    QElapsedTimer publicationTimer;
    publicationTimer.start();
    QList<ConfigPath> changed;
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
            if (m_cache.values.value(it.key()) != next)
                changed.append(it.key());
            m_cache.values[it.key()] = next;
            const auto latest = state.latestInput;
            state = it.value();
            state.latestInput = latest;
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
    ConfigConsumerGuard boundary(this);
    if (!changed.isEmpty() && !boundary.invoke([&] { emit valuesChanged(changed); }))
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
    if (packet.sourceError && !boundary.invoke([&] { emit sourceError(*packet.sourceError); }))
        return;
    if (packet.autoSaveRequested && m_accepting && m_status == Status::Ready)
        request("autosave", this, {});
    m_worstPublicationNanos = qMax(m_worstPublicationNanos, publicationTimer.nsecsElapsed());
}
void ConfigStore::receiveResult(const ConfigResult &result) {
    if (result.requestId < m_nextCompletion || !m_pending.contains(result.requestId))
        return;
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
