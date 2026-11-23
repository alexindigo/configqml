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
    static bool targetPresent(const ConfigLayers &layers, const ConfigPath &path, bool runtime) {
        for (const auto &layer : layers.m_layers)
            if (runtime ? layer.descriptor.kind == ConfigLayerKind::Runtime
                        : layer.descriptor.writable) {
                for (const auto &source : layer.sources)
                    if (startsWith(path, source.source.prefix))
                        return bool(
                            valueAt(source.source.raw, path.mid(source.source.prefix.size())));
                return false;
            }
        return false;
    }
    static QMap<ConfigPath, ConfigCandidateFact> capture(const ConfigLayers &layers,
                                                         const ConfigLayers &base,
                                                         const QList<ConfigPath> &paths,
                                                         bool runtime) {
        QMap<ConfigPath, ConfigCandidateFact> facts;
        const QSet<ConfigPath> selected(paths.cbegin(), paths.cend());
        QMap<ConfigPath, QString> masks;
        const auto values = layers.effectiveLeaves(&selected, &masks);
        for (const auto &path : paths) {
            ConfigCandidateFact fact;
            fact.effective = values.value(path);
            fact.maskedBy = masks.value(path);
            fact.targetPresent = targetPresent(base, path, runtime);
            facts.insert(path, fact);
        }
        return facts;
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
    const auto leaves = schema.leaves();
    // The schema owns the canonical, distinct leaf paths. A request for that
    // exact allowed selection does not need another normalization or path copy.
    if (schema.valid() && !leaves.isEmpty() && allowed == leaves && paths == leaves) {
        selection.leaves = leaves;
        return selection;
    }
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
    for (const auto &leaf : leaves)
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
    result.facts =
        ConfigLayerEditAccess::capture(*candidate, base, batch.selection.leaves, runtime);
    QSet<ConfigPath> policyPaths;
    for (const auto &policy : policies)
        policyPaths.insert(policy.path);
    for (auto it = result.facts.begin(); it != result.facts.end(); ++it) {
        const auto &path = it.key();
        auto &fact = it.value();
        const bool masked = !fact.maskedBy.isEmpty();
        if (runtime)
            fact.maskedBy.clear();
        const auto operation = batch.operations.constFind(path);
        if (operation != batch.operations.cend()) {
            fact.edited = true;
            fact.pending = operation->kind;
            fact.authored = operation->value;
        }
        // Publication validates the file-layer value even if runtime hides it.
        // A mask controls presentation, not admission of the underlying value.
        if (!policyPaths.contains(path))
            continue;
        if (const auto value = candidate->unmaskedValue(path)) {
            const auto projected = projectCandidateLeaf(path, *value, policies, metadata);
            if (!projected.ok()) {
                result.error = projected.errors.first().code;
                return result;
            }
            if (!masked)
                fact.effective = projected.value;
        }
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
QSet<ConfigPath> candidateTargetPresence(const ConfigLayers &layers, const QList<ConfigPath> &paths,
                                         bool runtime) {
    QSet<ConfigPath> present;
    for (const auto &path : paths)
        if (ConfigLayerEditAccess::targetPresent(layers, path, runtime))
            present.insert(path);
    return present;
}
} // namespace configqml
