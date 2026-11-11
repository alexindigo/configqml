// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"

namespace configqml {
ConfigCommand ConfigWorker::publicationOrigin() const {
    ConfigCommand origin{m_save.requestId, m_save.prefix, m_save.operation, {}};
    origin.groupToken = m_save.groupToken;
    if (!origin.groupToken.isEmpty())
        origin.evidenceAttemptId = m_groups.value(origin.groupToken).info.attempts.last().attemptId;
    return origin;
}
void ConfigWorker::rememberOwnPublication(const QString &source, const ConfigBackendResult &outcome,
                                          const QByteArray &bytes,
                                          const ConfigSourcePreimage &predecessor) {
    if (!outcome.publicationWitness)
        return;
    OwnPublication publication;
    publication.output = {*outcome.publicationWitness, bytes};
    publication.predecessor = predecessor;
    publication.origin = publicationOrigin();
    for (const auto &intent : m_saveIntents.value(source))
        if (!intent.edit.pruneEmpty)
            publication.intents.insert(intent.id);
    m_ownPublications[source][m_save.groupToken] = std::move(publication);
}
std::optional<ConfigWorker::OwnPublication>
ConfigWorker::ownPublication(const QString &source, const ConfigBackendResult &read) const {
    if (read.success)
        for (const auto &publication : m_ownPublications.value(source))
            if (publication.output.revision == read.revision &&
                publication.output.bytes == read.bytes)
                return publication;
    return {};
}
bool ConfigWorker::samePublicationBase(const QString &source, const ConfigSourcePreimage &a,
                                       const ConfigSourcePreimage &b) const {
    return m_files.contains(source) ? sameSourcePreimage(a, b)
                                    : a.revision == b.revision && a.bytes == b.bytes;
}
bool ConfigWorker::savePredecessor(const QString &source,
                                   const ConfigSourcePreimage &preimage) const {
    return samePublicationBase(source, preimage,
                               {m_saveRevisions.value(source), m_saveBytes.value(source)}) ||
           (m_recoveryPreimages.contains(source) &&
            samePublicationBase(source, preimage, m_recoveryPreimages.value(source)));
}
void ConfigWorker::advanceOwnPublications(const QString &source, const ConfigBackendResult &outcome,
                                          const QSet<quint64> &confirmedIntents) {
    auto found = m_ownPublications.find(source);
    if (found == m_ownPublications.end())
        return;
    auto &publications = found.value();
    for (auto it = publications.begin(); it != publications.end();) {
        if (it.key() == m_save.groupToken &&
            (!it.key().isEmpty() || (it->intents - confirmedIntents).isEmpty())) {
            it = publications.erase(it);
            continue;
        }
        const bool advance =
            it.key().isEmpty()
                ? savePredecessor(source, it->predecessor) || savePredecessor(source, it->output)
                : m_groups.value(it.key()).revisions.value(source) == outcome.revision &&
                      m_groups.value(it.key()).bytes.value(source) == outcome.bytes;
        if (advance) {
            // Only a causally checked own descendant advances an unsettled owner.
            // Its actual writer correlation remains distinct from that owner key.
            it->output = it->predecessor = {outcome.revision, outcome.bytes};
            it->origin = publicationOrigin();
        }
        ++it;
    }
    if (publications.isEmpty())
        m_ownPublications.erase(found);
}
} // namespace configqml
