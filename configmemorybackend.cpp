// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configmemorybackend.h"

namespace configqml {
static QByteArray copy(const QByteArray &bytes) {
    return QByteArray(bytes.constData(), bytes.size());
}
ConfigMemoryBackend::ConfigMemoryBackend() : m_owner(QThread::currentThreadId()) {}
bool ConfigMemoryBackend::onOwner() const { return m_owner == QThread::currentThreadId(); }
ConfigBackendCapabilities ConfigMemoryBackend::capabilities() const {
    return {true, true, false, true, ConfigBackendCapabilities::Affinity::SingleOwner};
}
bool ConfigMemoryBackend::schedule(const Schedule &schedule) {
    if (!onOwner())
        return false;
    m_schedules.append(schedule);
    return true;
}
bool ConfigMemoryBackend::seed(const QString &sourceId, const QByteArray &bytes, bool exists) {
    if (!onOwner() || sourceId.isEmpty())
        return false;
    m_entries.insert(sourceId, {exists ? copy(bytes) : QByteArray(),
                                {exists, QByteArray::number(++m_nextRevision)}});
    return true;
}
quint64 ConfigMemoryBackend::submit(const ConfigBackendRequest &input) {
    if (!onOwner())
        return 0; // Reserved invalid request ID; no mutation on foreign threads.
    const quint64 id = ++m_nextRequest;
    auto request = input;
    request.sourceId = QString(input.sourceId.constData(), input.sourceId.size());
    request.bytes = copy(input.bytes);
    request.expected.token = copy(input.expected.token);
    const auto schedule = m_schedules.isEmpty() ? Schedule{} : m_schedules.takeFirst();
    const Pending pending{request, schedule};
    if (schedule.delayed)
        m_pending.insert(id, pending);
    else
        m_completed.insert(id, execute(id, pending));
    return id;
}
std::optional<ConfigBackendResult> ConfigMemoryBackend::take(quint64 id) {
    if (!onOwner() || !m_completed.contains(id))
        return std::nullopt;
    return m_completed.take(id);
}
QList<quint64> ConfigMemoryBackend::pending() const {
    return onOwner() ? m_pending.keys() : QList<quint64>{};
}
bool ConfigMemoryBackend::release(quint64 id) {
    if (!onOwner() || !m_pending.contains(id))
        return false;
    const auto operation = m_pending.take(id);
    m_completed.insert(id, execute(id, operation));
    return true;
}
ConfigBackendResult ConfigMemoryBackend::execute(quint64 id, const Pending &pending) {
    const auto &request = pending.request;
    const bool publish = request.operation == ConfigBackendRequest::Operation::Publish;
    ConfigBackendResult result;
    result.requestId = id;
    result.sourceId = request.sourceId;
    result.operation = request.operation;
    auto fail = [&](const char *code) {
        result.error = {QString::fromLatin1(code), request.sourceId, publish ? "publish" : "read"};
        return result;
    };
    if (request.sourceId.isEmpty())
        return fail("invalid-source");
    if (pending.schedule.fault == Fault::BeforeOperation)
        return fail("injected-before-operation");
    auto entry = m_entries.value(request.sourceId);
    result.revision = entry.revision;
    if (!publish) {
        result.bytes = entry.bytes;
        result.success = true;
        return result;
    }
    if (request.expected != entry.revision)
        return fail("revision-conflict");
    entry = {copy(request.bytes), {true, QByteArray::number(++m_nextRevision)}};
    m_entries.insert(request.sourceId, entry);
    if (pending.schedule.fault == Fault::AfterPublication ||
        pending.schedule.fault == Fault::Readback) {
        result.publicationState = ConfigBackendResult::PublicationState::Uncertain;
        return fail(pending.schedule.fault == Fault::Readback ? "injected-readback"
                                                              : "injected-after-publication");
    }
    result.revision = entry.revision;
    result.bytes = entry.bytes; // Confirmed publication/readback, not just captured input.
    result.success = true;
    return result;
}
} // namespace configqml
