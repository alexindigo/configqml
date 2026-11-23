// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configcandidate_p.h"

namespace configqml {
// The layer engine remains the only transaction/target owner. This private
// adapter adds runtime batches without enlarging its released public surface.
struct ConfigLayerEditAccess {
    static ConfigWriteResult apply(ConfigLayers &layers,
                                   const QMap<ConfigPath, std::optional<QVariant>> &edits,
                                   bool runtime) {
        return layers.applyTargetEdits(edits, runtime);
    }
    static ConfigRoute route(const ConfigLayers &layers, const ConfigPath &path, bool runtime) {
        for (const auto &layer : layers.m_layers)
            if (runtime ? layer.descriptor.kind == ConfigLayerKind::Runtime
                        : layer.descriptor.writable)
                return layers.route(path, layer.descriptor.id);
        return {};
    }
};
bool ConfigSelection::matchesSchema(const ConfigSchema &other) const {
    return schemaIdentity == other.m_identity;
}
ConfigSelection ConfigSelection::capture(const ConfigSchema &schema, const QList<ConfigPath> &paths,
                                         const QList<ConfigPath> &allowed, Target target,
                                         const QString &failure, bool emptyMeansAll,
                                         bool rejectDuplicates) {
    ConfigSelection selection;
    selection.schema = schema;
    selection.schemaIdentity = schema.m_identity;
    selection.target = target;
    QSet<ConfigPath> seen, included, allowedAncestors;
    // Index the allowed boundary once. Looking up every requested leaf against
    // every allowed leaf makes a complete selected capture quadratic in width.
    for (const auto &leaf : allowed)
        for (qsizetype depth = 0; depth <= leaf.size(); ++depth)
            allowedAncestors.insert(leaf.mid(0, depth));
    if (!schema.valid() || (paths.isEmpty() && !emptyMeansAll))
        selection.error = failure;
    for (const auto &query : paths) {
        if (!schema.descriptor(query) || (rejectDuplicates && seen.contains(query))) {
            selection.error = failure;
            break;
        }
        seen.insert(ownPath(query));
        if (!allowedAncestors.contains(query))
            selection.error = failure;
    }
    for (const auto &leaf : allowed) {
        if (paths.isEmpty() && emptyMeansAll)
            included.insert(leaf);
        else
            for (qsizetype depth = 0; depth <= leaf.size(); ++depth)
                if (seen.contains(leaf.mid(0, depth))) {
                    included.insert(leaf);
                    break;
                }
    }
    for (const auto &leaf : schema.leaves())
        if (included.contains(leaf))
            selection.leaves.append(leaf);
    if (selection.leaves.isEmpty())
        selection.error = failure;
    return selection;
}
ConfigEditBatch ConfigEditBatch::capture(const ConfigSelection &selection,
                                         const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    ConfigEditBatch batch;
    batch.selection = selection;
    batch.error = selection.error;
    for (auto it = edits.cbegin(); batch.error.isEmpty() && it != edits.cend(); ++it) {
        if (!selection.contains(it.key())) {
            batch.error = "edit-selection";
            break;
        }
        ConfigEdit edit;
        if (it.value()) {
            const auto checked = selection.schema.validate(it.key(), *it.value());
            if (!checked.ok()) {
                batch.error = checked.errors.first().code;
                break;
            }
            edit.kind = ConfigEdit::Kind::Set;
            edit.value = checked.value;
        }
        batch.operations.insert(ownPath(it.key()), edit);
    }
    return batch;
}
QMap<ConfigPath, std::optional<QVariant>> ConfigEditBatch::layerEdits() const {
    QMap<ConfigPath, std::optional<QVariant>> result;
    for (auto it = operations.cbegin(); it != operations.cend(); ++it)
        result[it.key()] =
            it->kind == ConfigEdit::Kind::Set ? std::optional<QVariant>(it->value) : std::nullopt;
    return result;
}
ConfigValueResult projectCandidateLeaf(const ConfigPath &path, const QVariant &unmasked,
                                       const QMap<QString, ConfigRecordListPolicySpec> &policies,
                                       const QMap<QString, ConfigRecordListMetadata> &metadata) {
    ConfigValueResult result{unmasked, {}};
    for (const auto &policy : policies) {
        if (policy.path != path)
            continue;
        const auto projected =
            projectRecordLists(policy, metadata.value(policy.policyId), result.value);
        if (!projected.error.isEmpty())
            return {{}, {{path, projected.error}}};
        result.value = projected.value;
    }
    return result;
}
ConfigCandidate resolveCandidate(const ConfigLayers &base, const ConfigEditBatch &batch,
                                 const QMap<QString, ConfigRecordListPolicySpec> &policies,
                                 const QMap<QString, ConfigRecordListMetadata> &metadata,
                                 const std::function<QString(const ConfigInput &)> &admission) {
    ConfigCandidate result;
    result.error = batch.error;
    if (result.error.isEmpty() && batch.selection.lifetime && !*batch.selection.lifetime)
        result.error = "edit-closed";
    if (result.error.isEmpty() && !batch.selection.matchesSchema(base.schema()))
        result.error = "edit-schema";
    if (result.error.isEmpty() && batch.selection.target != ConfigSelection::Target::UserLayer &&
        batch.selection.target != ConfigSelection::Target::RecordSlot &&
        batch.selection.target != ConfigSelection::Target::RuntimeLayer)
        result.error = "edit-target";
    if (!result.error.isEmpty())
        return result;
    auto candidate = std::make_unique<ConfigLayers>(base);
    const auto edits = batch.layerEdits();
    const bool runtime = batch.selection.target == ConfigSelection::Target::RuntimeLayer;
    if (admission)
        for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
            ConfigInput input;
            input.kind =
                runtime
                    ? (it.value() ? ConfigInput::Kind::Runtime : ConfigInput::Kind::ClearRuntime)
                    : (it.value() ? ConfigInput::Kind::User : ConfigInput::Kind::Reset);
            input.sequence = batch.sequence;
            input.path = it.key();
            input.value = it.value().value_or(QVariant());
            result.error = admission(input);
            if (!result.error.isEmpty())
                return result;
        }
    // An empty edit algebra observes the already admitted cut. Re-admitting its
    // raw sources would rerun transforms without an input change; selected facts
    // and current record-list projection still follow the common path below.
    if (!edits.isEmpty()) {
        const auto outcome = ConfigLayerEditAccess::apply(*candidate, edits, runtime);
        if (!outcome.accepted) {
            result.error =
                outcome.errors.isEmpty() ? QString("admission") : outcome.errors.first().code;
            return result;
        }
    }
    const auto effective = candidate->effective(); // Resolve the batch once, not per selected leaf.
    for (const auto &path : batch.selection.leaves) {
        ConfigCandidateFact fact;
        fact.effective = valueAt(effective, path).value_or(QVariant());
        const auto info = candidate->info(path);
        fact.maskedBy = runtime ? QString() : info.maskedBy;
        const auto target = ConfigLayerEditAccess::route(base, path, runtime);
        const auto source = base.source(target.layerId, target.sourceId);
        fact.targetPresent = source && bool(valueAt(source->raw, path.mid(source->prefix.size())));
        const auto operation = batch.operations.constFind(path);
        if (operation != batch.operations.cend()) {
            fact.edited = true;
            fact.pending = operation->kind;
            fact.authored = operation->value;
        }
        if (fact.effective.isValid() && info.maskedBy.isEmpty()) {
            const auto projected = projectCandidateLeaf(path, fact.effective, policies, metadata);
            if (!projected.ok()) {
                result.error = projected.errors.first().code;
                return result;
            }
            fact.effective = projected.value;
        }
        result.facts.insert(path, fact);
    }
    result.layers = std::move(candidate);
    return result;
}
QMap<ConfigPath, QVariant> candidatePresentation(const ConfigCandidate &candidate) {
    QMap<ConfigPath, QVariant> values;
    for (auto it = candidate.facts.cbegin(); it != candidate.facts.cend(); ++it)
        values[it.key()] =
            it->edited && it->pending == ConfigEdit::Kind::Set ? it->authored : it->effective;
    return values;
}
} // namespace configqml
