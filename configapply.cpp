// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"

namespace configqml {
QVariantList ConfigWorker::applySourceFacts(const ConfigPath &path) const {
    QVariantList facts;
    for (const auto &contribution : m_diskLayers->info(path).contributions) {
        const auto source = m_diskLayers->source(contribution.layerId, contribution.sourceId);
        if (!source)
            continue;
        const auto raw = valueAt(source->raw, path.mid(source->prefix.size()));
        facts.append(
            QVariantList{source->id, contribution.layerId, bool(raw), raw.value_or(QVariant())});
    }
    return facts;
}
void ConfigWorker::checkApplyCaptures() {
    for (auto it = m_applyCaptures.begin(); it != m_applyCaptures.end();) {
        const auto witness = it->witness.lock();
        if (!witness) {
            it = m_applyCaptures.erase(it);
            continue;
        }
        for (auto fence = it->fences.begin(); witness->valid && fence != it->fences.end();
             ++fence) {
            const auto current = m_previous.information.value(fence.key());
            const bool sameIntent = current.intentRevision == fence->intentRevision &&
                                    current.pathGeneration == fence->pathGeneration;
            if (sameIntent && it->ownConfirmation.contains(fence.key())) {
                // Only confirmed own IO may change the captured raw/content fence.
                fence.value() = current;
                it->sourceFacts[fence.key()] = applySourceFacts(fence.key());
            } else if (sameIntent && it->rejectedIngress.contains(fence.key()) &&
                       current.contentRevision == fence->contentRevision &&
                       applySourceFacts(fence.key()) == it->sourceFacts.value(fence.key())) {
                fence->publicationGeneration = current.publicationGeneration;
            } else if (!sameIntent || current.contentRevision != fence->contentRevision ||
                       (it->publicationFence &&
                        current.publicationGeneration != fence->publicationGeneration) ||
                       applySourceFacts(fence.key()) != it->sourceFacts.value(fence.key()))
                witness->valid = false;
        }
        it->ownConfirmation.clear();
        it->rejectedIngress.clear();
        ++it;
    }
}
void ConfigWorker::prepareApplyConfirmation(const QString &source,
                                            const ConfigSourcePreimage *preflight) {
    const auto route = m_routes.value(source);
    const auto prefix = route.prefix;
    const auto disk = m_diskLayers->source(route.layerId, source);
    const auto fresh =
        preflight ? ConfigJsonCodec().decode(preflight->bytes, source).raw : QVariantMap{};
    for (auto &capture : m_applyCaptures) {
        const auto witness = capture.witness.lock();
        if (!witness || !witness->valid)
            continue;
        for (auto it = capture.fences.cbegin(); it != capture.fences.cend(); ++it) {
            if (!startsWith(it.key(), prefix))
                continue;
            const auto relative = it.key().mid(prefix.size());
            const auto before = disk ? valueAt(disk->raw, relative) : std::nullopt;
            const auto after = valueAt(fresh, relative);
            const bool samePreflight =
                !preflight ||
                (bool(before) == bool(after) &&
                 (!before || encodeJsonValue(*before).bytes == encodeJsonValue(*after).bytes));
            if (m_pathIntents.value(it.key()) == it->intentRevision &&
                m_pathGenerations.value(it.key()) == it->pathGeneration &&
                m_contentRevisions.value(it.key()) == it->contentRevision &&
                applySourceFacts(it.key()) == capture.sourceFacts.value(it.key()) && samePreflight)
                capture.ownConfirmation.insert(it.key());
            else
                witness->valid = false;
        }
    }
}
ConfigApplyCapture *ConfigWorker::applyCapture(const ConfigCommand &command) {
    if (!command.applyAttempt || !command.applyAttempt->expected)
        return nullptr;
    checkApplyCaptures();
    auto found = m_applyCaptures.find(command.applyAttempt->ownerId);
    if (found == m_applyCaptures.end() || !command.applyAttempt->expected->valid ||
        found->witness.lock() != command.applyAttempt->expected)
        return nullptr;
    return &found.value();
}
void ConfigWorker::rememberApply(const ConfigCommand &command, const QList<ConfigPath> &paths,
                                 const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    if (!command.applyAttempt)
        return;
    auto witness = std::make_shared<ConfigApplyWitness>();
    ConfigApplyCapture capture;
    capture.witness = witness;
    capture.edits = edits;
    capture.publicationFence = command.applyAttempt->publicationFence;
    for (const auto &path : paths) {
        capture.fences[path] = m_previous.information.value(path);
        capture.sourceFacts[path] = applySourceFacts(path);
    }
    witness->fences = capture.fences;
    command.applyAttempt->admitted = std::move(witness);
    m_applyCaptures[command.applyAttempt->ownerId] = std::move(capture);
}
void ConfigWorker::reconcileApplyRejection(const ConfigCommand &command) {
    if (!command.input || !command.applyAttempt || !command.applyAttempt->expected)
        return;
    auto found = m_applyCaptures.find(command.applyAttempt->ownerId);
    if (found == m_applyCaptures.end() || found->witness.lock() != command.applyAttempt->expected ||
        !command.applyAttempt->expected->valid)
        return;
    for (auto it = command.input->pathGenerations.cbegin();
         it != command.input->pathGenerations.cend(); ++it) {
        auto fence = found->fences.find(it.key());
        if (fence != found->fences.end() && fence->pathGeneration + 1 == it.value() &&
            m_pathGenerations.value(it.key()) == it.value() &&
            m_pathIntents.value(it.key()) == fence->intentRevision &&
            m_contentRevisions.value(it.key()) == fence->contentRevision) {
            fence->pathGeneration = it.value();
            found->rejectedIngress.insert(it.key());
        }
    }
}
} // namespace configqml
