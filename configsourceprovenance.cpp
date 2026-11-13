// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "configrevision_p.h"

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
    // Recovery verified this original capture against an exact own predecessor.
    // An uncertain descendant must keep that root for the next Retry too.
    publication.predecessor = predecessor;
    publication.recoveryBase = m_recoveryPreimages.value(source, predecessor);
    publication.origin = publicationOrigin();
    for (const auto &intent : m_saveIntents.value(source))
        if (!intent.edit.pruneEmpty)
            publication.intents.insert(intent.id);
    auto &publications = m_ownPublications[source];
    for (auto &prior : publications)
        if (prior.output.revision == predecessor.revision &&
            prior.output.bytes == predecessor.bytes) {
            // A completed attempt witnessed a descendant of this exact version.
            // Keep every pending owner on that frontier without covering intent
            // or settling any stopped group before successful confirmation.
            prior.output = publication.output;
            prior.origin = publication.origin;
        }
    publications[m_save.groupToken] = std::move(publication);
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
bool ConfigWorker::causalPublicationBase(const QString &source, const ConfigSourcePreimage &a,
                                         const ConfigSourcePreimage &b) const {
    if (samePublicationBase(source, a, b))
        return true;
    const auto left = a.revision.witness, right = b.revision.witness;
    if (!m_files.contains(source) || !left || !right || left->target != right->target ||
        a.revision.exists != b.revision.exists || a.bytes != b.bytes)
        return false;
    // This is an own causal link, never equality with a foreign read. Only
    // descriptors retained by the attempted capture can justify the extension.
    return (!left->ownedParents.isEmpty() && left->ownedParentBase == right->parents) ||
           (!right->ownedParents.isEmpty() && right->ownedParentBase == left->parents);
}
bool ConfigWorker::savePredecessor(const QString &source,
                                   const ConfigSourcePreimage &preimage) const {
    if (causalPublicationBase(source, preimage,
                              {m_saveRevisions.value(source), m_saveBytes.value(source)}) ||
        (m_recoveryPreimages.contains(source) &&
         causalPublicationBase(source, preimage, m_recoveryPreimages.value(source))))
        return true;
    for (const auto &publication : m_ownPublications.value(source))
        if (publication.output.revision == m_saveRevisions.value(source) &&
            publication.output.bytes == m_saveBytes.value(source) &&
            (causalPublicationBase(source, preimage, publication.predecessor) ||
             causalPublicationBase(source, preimage, publication.recoveryBase)))
            return true;
    return false;
}
void ConfigWorker::supersedeOrdinaryPublication(const QString &source,
                                                const ConfigBackendResult &read) {
    // An uncertainty witness classifies exact own output; it is not an extra
    // conflict policy. Call only after retaining this successful observation.
    if (!read.success || !m_files.contains(source) ||
        m_files.value(source).conflictPolicy != ConfigConflictPolicy::PathIntent)
        return;
    auto found = m_ownPublications.find(source);
    if (found == m_ownPublications.end())
        return;
    auto &publications = found.value();
    const auto ordinary = publications.constFind(QString());
    if (ordinary != publications.cend() &&
        (ordinary->output.revision != read.revision || ordinary->output.bytes != read.bytes)) {
        publications.remove(QString());
        if (publications.isEmpty())
            m_ownPublications.erase(found);
    }
}
void ConfigWorker::advanceOwnPublications(const QString &source, const ConfigBackendResult &outcome,
                                          const QSet<quint64> &confirmedIntents) {
    auto found = m_ownPublications.find(source);
    if (found == m_ownPublications.end())
        return;
    auto &publications = found.value();
    QSet<quint64> pending;
    for (const auto &intent : m_intents.value(source))
        pending.insert(intent.id);
    for (auto it = publications.begin(); it != publications.end();) {
        if ((it.key().isEmpty() && (it->intents & pending).isEmpty()) ||
            (it.key() == m_save.groupToken &&
             (!it.key().isEmpty() || (it->intents - confirmedIntents).isEmpty()))) {
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
            it->output = it->predecessor = it->recoveryBase = {outcome.revision, outcome.bytes};
            it->origin = publicationOrigin();
        }
        ++it;
    }
    if (publications.isEmpty())
        m_ownPublications.erase(found);
}
} // namespace configqml
