// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include <QCryptographicHash>
#include <QFileInfo>

namespace configqml {
void ConfigWorker::attachEvidence(const PendingEvidence &pending,
                                  const ConfigEvidenceReference &reference, ConfigResult *result) {
    for (auto &state : m_sourceStates)
        if (state.sourceId == pending.observation.sourceId)
            state.evidence.append(reference);
    if (result)
        result->evidence.append(reference);
    // A recovered entry keeps its original correlation, never another attempt's
    // deduplication identity. Different attempts intentionally retain separately.
    if (result == &m_saveResult && !pending.attemptKey.isEmpty() &&
        pending.observation.requestId == QString::number(m_save.requestId))
        m_attemptEvidence.insert(pending.attemptKey, reference);
}
QString ConfigWorker::drainEvidence(const QString &source, ConfigResult *result) {
    auto it = m_pendingEvidence.find(source);
    if (it != m_pendingEvidence.end()) {
        auto &queue = it.value();
        while (!queue.isEmpty()) {
            const auto injected = m_options.fileFault
                                      ? m_options.fileFault("evidence-retain", m_json->path(source))
                                      : QString();
            const auto reference = injected.isEmpty() ? retainEvidence(m_options.evidenceDirectory,
                                                                       queue.head().observation)
                                                      : std::nullopt;
            if (!reference)
                return m_evidenceBlocked.contains(source) ? "evidence-backlog"
                                                          : "evidence-retention";
            attachEvidence(queue.head(), *reference, result);
            queue.dequeue(); // Only a durable complete artifact retires acquired bytes.
        }
        m_pendingEvidence.erase(it);
    }
    m_evidenceBlocked.remove(source);
    return {};
}
QString ConfigWorker::prepareEvidence(const QString &source, qsizetype required,
                                      ConfigResult *result) {
    if (m_options.evidenceDirectory.isEmpty())
        return {};
    drainEvidence(source, result);
    const auto pending = m_pendingEvidence.value(source).size();
    if (m_evidenceBlocked.contains(source) || pending + required > EvidenceBacklogLimit) {
        m_evidenceBlocked.insert(source);
        return "evidence-backlog";
    }
    return {};
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
    QByteArray attemptKey;
    if (command && command->requestId == m_save.requestId && result == &m_saveResult) {
        attemptKey =
            QCryptographicHash::hash(encodeMemoryValue({{"source", id},
                                                        {"phase", phase},
                                                        {"role", role},
                                                        {"presence", observation.presence},
                                                        {"bytesState", observation.bytesState},
                                                        {"revision", read.revision.token},
                                                        {"exists", read.revision.exists},
                                                        {"bytes", read.bytes}}),
                                     QCryptographicHash::Sha256);
        if (m_attemptEvidence.contains(attemptKey))
            return {};
    }
    if (command) {
        observation.requestId =
            command->requestId ? QString::number(command->requestId) : QString();
        observation.groupToken = command->groupToken;
        if (!command->evidenceAttemptId.isEmpty())
            observation.attemptId = command->evidenceAttemptId;
        else if (!command->groupToken.isEmpty())
            observation.attemptId =
                m_groups.value(command->groupToken).info.attempts.isEmpty()
                    ? QString()
                    : m_groups.value(command->groupToken).info.attempts.last().attemptId;
    }
    if (!attemptKey.isEmpty())
        for (const auto &pending : m_pendingEvidence.value(id))
            if (pending.attemptKey == attemptKey &&
                pending.observation.requestId == observation.requestId)
                return drainEvidence(id, result);
    auto &queue = m_pendingEvidence[id];
    // Each acquisition boundary reserves its worst-case observation count before
    // reading/publishing. A transaction owns those slots until synchronous return.
    if (queue.size() >= EvidenceBacklogLimit) {
        m_evidenceBlocked.insert(id);
        return "evidence-backlog";
    }
    observation.observationOrder = ++m_observationOrder;
    queue.enqueue({std::move(observation), attemptKey});
    if (queue.size() == EvidenceBacklogLimit)
        m_evidenceBlocked.insert(id);
    return drainEvidence(id, result);
}
} // namespace configqml
