// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlayers.h"
#include "configjsoncodec.h"
#include "configsourceprojection_p.h"
#include <QSet>
#include <limits>
#include <algorithm>

namespace configqml {
ConfigLayers::ConfigLayers(const ConfigSchema &schema, const QList<ConfigLayer> &layers,
                           const QList<ConfigProjectionSpec> &projections)
    : m_projections(projections), m_schema(schema), m_errors(schema.errors()) {
    QSet<QString> ids;
    int previous = -1;
    int writable = 0;
    QSet<int> singletonKinds;
    for (const auto &layer : layers) {
        const int kind = static_cast<int>(layer.kind);
        if (layer.id.isEmpty() || layer.id == "defaults" || ids.contains(layer.id) ||
            kind < previous || (kind != 0 && singletonKinds.contains(kind)) ||
            (layer.writable && layer.kind != ConfigLayerKind::File))
            m_errors.append(m_schema.diagnostics().make({}, "layer-descriptor"));
        previous = kind;
        singletonKinds.insert(kind);
        ids.insert(layer.id);
        writable += layer.writable;
        LayerState state{layer, {}};
        state.descriptor.id = QString(layer.id.constData(), layer.id.size());
        state.descriptor.sources.clear(); // Only one owned raw copy, in SourceState.
        QSet<QString> sourceIds;
        for (const auto &source : layer.sources) {
            const auto d = schema.descriptor(source.prefix);
            if (source.id.isEmpty() || sourceIds.contains(source.id) || !d ||
                d->type != ConfigDescriptor::Type::Group)
                m_errors.append(m_schema.diagnostics().make(source.prefix, "source-descriptor"));
            for (const auto &other : state.sources)
                if (startsWith(source.prefix, other.source.prefix) ||
                    startsWith(other.source.prefix, source.prefix))
                    m_errors.append(
                        m_schema.diagnostics().make(source.prefix, "overlapping-sources"));
            sourceIds.insert(source.id);
            SourceState owned;
            owned.source.id = ownString(source.id);
            owned.source.prefix = ownPath(source.prefix);
            const auto result = admit(state, owned, source);
            // Schema-invalid input is retained as raw with diagnostics, not a broken store.
            if (!result.accepted && owned.errors.isEmpty())
                m_errors.append(result.errors);
            state.sources.append(owned);
        }
        m_layers.append(state);
    }
    if (writable > 1)
        m_errors.append(m_schema.diagnostics().make({}, "multiple-writable-layers"));
    QMap<QString, QPair<qsizetype, const SourceState *>> sources;
    QMap<QString, int> sourceCounts;
    for (qsizetype i = 0; i < m_layers.size(); ++i)
        for (const auto &source : m_layers[i].sources) {
            ++sourceCounts[source.source.id];
            if (sources.contains(source.source.id) && !source.source.fallbackFor.isEmpty())
                m_errors.append(m_schema.diagnostics().make({}, "fallback-declaration"));
            sources[source.source.id] = {i, &source};
        }
    for (qsizetype i = 0; i < m_layers.size(); ++i)
        for (const auto &source : m_layers[i].sources) {
            if (source.source.fallbackFor.isEmpty())
                continue;
            const auto target = sources.constFind(source.source.fallbackFor);
            if (sourceCounts.value(source.source.id) != 1 ||
                sourceCounts.value(source.source.fallbackFor) != 1 ||
                m_layers[i].descriptor.writable ||
                m_layers[i].descriptor.kind != ConfigLayerKind::File || target == sources.cend() ||
                target->first <= i ||
                m_layers[target->first].descriptor.kind != ConfigLayerKind::File ||
                !target->second->source.fallbackFor.isEmpty() ||
                !startsWith(source.source.prefix, target->second->source.prefix))
                m_errors.append(m_schema.diagnostics().make({}, "fallback-declaration"));
        }
    QSet<QString> projectionIds;
    for (auto &projection : m_projections) {
        projection.projectionId = ownString(projection.projectionId);
        for (auto &id : projection.sourceIds)
            id = ownString(id);
        const auto metadata = ownValue(projection.metadata);
        projection.metadata = metadata.value.toMap();
        QSet<QString> selected;
        bool invalid = projection.projectionId.isEmpty() ||
                       projectionIds.contains(projection.projectionId) ||
                       projection.sourceIds.isEmpty() || !projection.transform || !metadata.ok();
        projectionIds.insert(projection.projectionId);
        for (const auto &id : projection.sourceIds) {
            invalid |= selected.contains(id) || sourceCounts.value(id) != 1 ||
                       !sources.contains(id) ||
                       (sources.contains(id) &&
                        m_layers[sources[id].first].descriptor.kind != ConfigLayerKind::File);
            selected.insert(id);
        }
        if (invalid)
            m_errors.append(m_schema.diagnostics().make({}, "projection-declaration"));
    }
    if (valid())
        projectSources();
}
bool ConfigLayers::participates(const SourceState &source) const {
    if (!source.source.available)
        return false;
    return source.source.fallbackFor.isEmpty() || fallbackState(source.source.id) == "active";
}
void ConfigLayers::removeTransientLayers() {
    m_layers.removeIf(
        [](const LayerState &layer) { return layer.descriptor.kind != ConfigLayerKind::File; });
}
QString ConfigLayers::fallbackState(const QString &sourceId) const {
    const SourceState *fallback = nullptr;
    for (const auto &layer : m_layers)
        for (const auto &source : layer.sources)
            if (source.source.id == sourceId)
                fallback = &source;
    if (!fallback || fallback->source.fallbackFor.isEmpty())
        return "ordinary";
    for (const auto &layer : m_layers)
        for (const auto &target : layer.sources)
            if (target.source.id == fallback->source.fallbackFor) {
                if (!target.source.available)
                    return "unknown";
                if (!target.source.exists)
                    return "active";
                QVariant node = target.source.raw;
                for (const auto &segment :
                     fallback->source.prefix.mid(target.source.prefix.size())) {
                    if (node.metaType().id() != QMetaType::QVariantMap)
                        return "suppressed";
                    const auto map = node.toMap();
                    if (!map.contains(segment))
                        return "active";
                    node = map.value(segment);
                }
                return node.metaType().id() == QMetaType::QVariantMap && node.toMap().isEmpty()
                           ? "active"
                           : "suppressed";
            }
    return "unknown";
}
QList<ConfigProjectionEffectsEvent> ConfigLayers::takeProjectionEffects() {
    auto effects = std::move(m_effects);
    m_effects.clear();
    return effects;
}
QList<QPair<QString, ConfigDiagnostic>> ConfigLayers::takeProjectionDiagnostics() {
    auto diagnostics = std::move(m_projectionDiagnostics);
    m_projectionDiagnostics.clear();
    return diagnostics;
}
namespace {
QVariantMap sourceOverlay(const ConfigDescriptor &descriptor, QVariantMap lower,
                          const QVariantMap &raw) {
    for (auto it = raw.cbegin(); it != raw.cend(); ++it) {
        const auto field = descriptor.fields.constFind(it.key());
        if (field != descriptor.fields.cend() && field->type == ConfigDescriptor::Type::Group &&
            it->metaType().id() == QMetaType::QVariantMap)
            lower[it.key()] = sourceOverlay(*field, lower.value(it.key()).toMap(), it->toMap());
        else
            lower[it.key()] = it.value();
    }
    return lower;
}
} // namespace
void ConfigLayers::projectSources(const QList<LayerState> *previous) {
    auto lower = m_schema.defaults();
    for (qsizetype layerIndex = 0; layerIndex < m_layers.size(); ++layerIndex) {
        auto &layer = m_layers[layerIndex];
        const auto lowerBefore = lower;
        QList<ConfigProjectionEffectsEvent> layerEvents;
        QList<QPair<QString, ConfigDiagnostic>> layerWarnings;
        for (auto &state : layer.sources) {
            const auto original = state.source;
            if (!participates(state))
                continue;
            bool selected = false;
            for (const auto &spec : m_projections)
                selected |= spec.sourceIds.contains(original.id);
            if (selected && (original.exists || original.localIntent)) {
                ConfigProjectionInput input;
                input.raw = original.raw;
                const auto prior = input.raw.take("_configqmlVersion");
                if (prior.isValid()) {
                    const auto checked =
                        ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)
                            .validate(prior);
                    if (!checked.ok() || checked.value.toLongLong() < 0 ||
                        checked.value.toLongLong() > std::numeric_limits<int>::max()) {
                        state.projectionError = "projection-result";
                        state.rejected = true;
                        state.errors = {
                            m_schema.diagnostics().make(original.prefix, "projection-result")};
                        continue;
                    }
                    input.priorVersion = checked.value.toInt();
                }
                input.source = {original.id, layer.descriptor.id, original.prefix,
                                original.diskExists.value_or(original.exists) ? "present"
                                                                              : "absent",
                                QString::fromLatin1(original.revision.toHex())};
                input.schemaVersion = m_schema.version();
                input.defaults =
                    valueAt(m_schema.defaults(), original.prefix).value_or(QVariantMap{}).toMap();
                input.lowerValues =
                    valueAt(lowerBefore, original.prefix).value_or(QVariantMap{}).toMap();
                input.values = sourceOverlay(*m_schema.descriptor(original.prefix),
                                             input.lowerValues, input.raw);
                input.compatibilityRaw = input.raw;
                QList<ConfigProjectionEffectsEvent> events;
                QList<ConfigDiagnostic> warnings;
                QString error;
                for (const auto &spec : m_projections) {
                    if (!spec.sourceIds.contains(original.id))
                        continue;
                    input.metadata = spec.metadata;
                    ConfigProjectionResult output;
                    try {
                        output = spec.transform(input);
                    } catch (const ConfigProjectionFailure &failure) {
                        error = failure.code;
                        break;
                    } catch (...) {
                        error = "projection-script";
                        break;
                    }
                    const auto values = ownValue(output.values);
                    const auto compatibility = ownValue(output.compatibilityRaw);
                    if (!values.ok() || !compatibility.ok()) {
                        const auto code = !values.ok() ? values.errors.first().code
                                                       : compatibility.errors.first().code;
                        error = code == "value-depth"  ? "projection-depth"
                                : code == "non-finite" ? "projection-number"
                                                       : "projection-result";
                        break;
                    }
                    if (output.effects.size() > 64 || output.diagnostics.size() > 64) {
                        error = output.effects.size() > 64 ? "projection-effects"
                                                           : "projection-diagnostics";
                        break;
                    }
                    QSet<QString> effectIds;
                    QVariantList encodedEffects;
                    for (auto &effect : output.effects) {
                        const auto arguments = ownValue(effect.arguments);
                        if (effect.effectId.isEmpty() || effect.action.isEmpty() ||
                            effect.version > 9007199254740991ULL ||
                            effectIds.contains(effect.effectId) || !arguments.ok()) {
                            error = "projection-effects";
                            break;
                        }
                        effectIds.insert(effect.effectId);
                        effect.effectId = ownString(effect.effectId);
                        effect.action = ownString(effect.action);
                        effect.arguments = arguments.value.toMap();
                        encodedEffects.append(QVariantMap{{"effectId", effect.effectId},
                                                          {"action", effect.action},
                                                          {"version", effect.version},
                                                          {"arguments", arguments.value}});
                    }
                    const auto encoded = encodeJsonValue(encodedEffects);
                    if (!encoded.ok() || encoded.bytes.size() > 65536)
                        error = "projection-effects";
                    for (const auto &diagnostic : output.diagnostics)
                        if (diagnostic.code.isEmpty() ||
                            (diagnostic.severity != "warning" && diagnostic.severity != "error") ||
                            diagnostic.severity == "error")
                            error = "projection-diagnostics";
                        else
                            warnings.append(m_schema.diagnostics()
                                                .at(original.prefix)
                                                .make(diagnostic.path, diagnostic.code));
                    if (!error.isEmpty())
                        break;
                    input.values = values.value.toMap();
                    input.compatibilityRaw = compatibility.value.toMap();
                    const auto run = QString::fromLatin1(
                        encodeJsonValue(
                            QVariantList{spec.projectionId, original.id, input.source.revision})
                            .bytes.toHex());
                    if (!m_effectRuns.contains(run))
                        events.append({run, spec.projectionId, original.id, input.source.revision,
                                       output.effects});
                }
                if (error.isEmpty()) {
                    auto candidate = original;
                    candidate.raw = input.values;
                    candidate.exists = true;
                    auto previous = state;
                    const auto accepted = admit(layer, state, candidate, true);
                    state.source = original;
                    if (!accepted.accepted)
                        state.admitted = previous.admitted;
                    else {
                        state.projectionError.clear();
                        layerEvents.append(events);
                        for (const auto &warning : warnings)
                            layerWarnings.append({original.id, warning});
                    }
                } else {
                    state.projectionError = error;
                    state.rejected = true;
                    state.errors = {m_schema.diagnostics().make(original.prefix, error)};
                }
            }
        }
        bool rejected = false;
        for (const auto &state : layer.sources)
            rejected |= participates(state) && state.rejected;
        if (rejected) {
            for (qsizetype i = 0; i < layer.sources.size(); ++i)
                layer.sources[i].admitted = previous ? (*previous)[layerIndex].sources[i].admitted
                                                     : QMap<ConfigPath, AdmittedValue>{};
        } else {
            for (const auto &event : layerEvents) {
                m_effectRuns.insert(event.runId);
                m_effects.append(event);
            }
            m_projectionDiagnostics.append(layerWarnings);
        }
        lower = lowerBefore;
        for (const auto &state : layer.sources)
            if (participates(state))
                for (auto it = state.admitted.cbegin(); it != state.admitted.cend(); ++it)
                    putValue(lower, it.key(), it->value);
    }
}
bool ConfigLayers::rejects(const LayerState &layer, const ConfigPath &path) const {
    const auto policy = m_schema.policy(path);
    return policy == ConfigDescriptor::AdmissionPolicy::Strict ||
           (policy == ConfigDescriptor::AdmissionPolicy::Inherit &&
            (layer.descriptor.policy == ConfigAdmissionPolicy::RejectInput ||
             layer.descriptor.shippedDefaults));
}

ConfigOperation ConfigLayers::admit(LayerState &layer, SourceState &state,
                                    const ConfigSource &input, bool projected) {
    const auto captured = ownValue(input.raw, input.prefix, m_schema.diagnostics());
    if (!captured.ok())
        return {false, captured.errors};
    ConfigSource source = input;
    source.id = QString(input.id.constData(), input.id.size());
    source.prefix = ownPath(input.prefix);
    source.raw = captured.value.toMap();
    source.bytes = QByteArray(input.bytes.constData(), input.bytes.size());
    source.revision = QByteArray(input.revision.constData(), input.revision.size());
    source.fallbackFor = ownString(input.fallbackFor);
    if (!projected)
        for (const auto &projection : m_projections)
            if (projection.sourceIds.contains(source.id)) {
                state.source = source;
                state.errors.clear();
                state.rejected = false;
                state.projectionError.clear();
                if (!source.exists && !source.localIntent)
                    state.admitted.clear();
                return {true, {}};
            }
    QMap<ConfigPath, AdmittedValue> admitted;
    QList<ConfigDiagnostic> errors;
    if (source.exists) {
        for (const auto &path : m_schema.leaves()) {
            if (!startsWith(path, source.prefix))
                continue;
            const auto relative = path.mid(source.prefix.size());
            QVariant node = source.raw;
            bool missing = false;
            bool invalidNamespace = false;
            ConfigPath traversed = source.prefix;
            for (const auto &key : relative) {
                if (node.metaType().id() != QMetaType::QVariantMap) {
                    bool reported = false;
                    for (const auto &error : errors)
                        reported |= error.path == traversed;
                    if (!reported)
                        errors.append(m_schema.diagnostics().make(traversed, "namespace-type"));
                    missing = true;
                    invalidNamespace = true;
                    break;
                }
                const auto map = node.toMap();
                if (!map.contains(key)) {
                    missing = true;
                    break;
                }
                node = map.value(key);
                traversed.append(key);
            }
            if (missing) {
                if (m_schema.policy(path) == ConfigDescriptor::AdmissionPolicy::Inherit &&
                    layer.descriptor.policy == ConfigAdmissionPolicy::RetainLastGood &&
                    invalidNamespace && state.admitted.contains(path))
                    admitted.insert(path, state.admitted.value(path));
                continue;
            }
            const auto checked = m_schema.validate(
                path, source.json ? jsonSettingValue(node, *m_schema.descriptor(path)) : node);
            if (checked.ok())
                admitted.insert(path, {checked.value, source.revision, 0});
            else {
                errors.append(checked.errors);
                if (m_schema.policy(path) == ConfigDescriptor::AdmissionPolicy::Inherit &&
                    layer.descriptor.policy == ConfigAdmissionPolicy::RetainLastGood &&
                    state.admitted.contains(path))
                    admitted.insert(path, state.admitted.value(path));
            }
        }
    }
    state.source = source;
    state.errors = errors;
    state.rejected = false;
    for (const auto &error : errors)
        state.rejected |= rejects(layer, error.path);
    if (state.rejected)
        return {false, errors}; // Keep prior projection and new raw evidence.
    state.admitted = admitted;
    return {true, errors};
}

ConfigOperation ConfigLayers::replaceSource(const QString &layerId, const ConfigSource &source) {
    return replaceSources(layerId, {source});
}
ConfigOperation ConfigLayers::replaceSources(const QString &layerId,
                                             const QList<ConfigSource> &sources) {
    return replaceAllSources({{layerId, sources}});
}
ConfigOperation ConfigLayers::replaceAllSources(const QMap<QString, QList<ConfigSource>> &sources) {
    if (!valid())
        return {false, m_errors};
    auto candidate = *this;
    ConfigOperation result{true, {}};
    for (auto input = sources.cbegin(); input != sources.cend(); ++input) {
        auto layer = std::find_if(
            candidate.m_layers.begin(), candidate.m_layers.end(),
            [&](const LayerState &state) { return state.descriptor.id == input.key(); });
        if (layer == candidate.m_layers.end())
            return {false, {m_schema.diagnostics().make({}, "unknown-source")}};
        QSet<QString> seen;
        for (const auto &source : input.value()) {
            bool found = false;
            if (seen.contains(source.id))
                return {false, {m_schema.diagnostics().make(source.prefix, "duplicate-source")}};
            seen.insert(source.id);
            for (auto &state : layer->sources)
                if (state.source.id == source.id) {
                    found = true;
                    if (state.source.prefix != source.prefix)
                        return {false,
                                {m_schema.diagnostics().make(source.prefix, "source-topology")}};
                    const auto admitted = candidate.admit(*layer, state, source);
                    result.accepted &= admitted.accepted;
                    result.errors.append(admitted.errors);
                }
            if (!found)
                return {false, {m_schema.diagnostics().make({}, "unknown-source")}};
        }
    }
    candidate.projectSources(&m_layers);
    const auto rejected = candidate.rejectedDiagnostics();
    if (!rejected.isEmpty()) {
        result.accepted = false;
        result.errors.append(rejected);
    }
    *this = std::move(candidate);
    return result;
}

QVariantMap ConfigLayers::effective() const {
    if (!valid())
        return {};
    auto out = m_schema.defaults();
    for (const auto &layer : m_layers)
        for (const auto &source : layer.sources)
            if (participates(source))
                for (auto it = source.admitted.cbegin(); it != source.admitted.cend(); ++it)
                    putValue(out, it.key(), it.value().value);
    return out;
}
std::optional<QVariant> ConfigLayers::value(const ConfigPath &path) const {
    if (!valid() || !m_schema.descriptor(path))
        return std::nullopt;
    return valueAt(effective(), path);
}
std::optional<QVariant> ConfigLayers::unmaskedValue(const ConfigPath &path) const {
    if (!valid() || !m_schema.descriptor(path))
        return {};
    auto value = valueAt(m_schema.defaults(), path);
    for (const auto &layer : m_layers)
        if (layer.descriptor.kind == ConfigLayerKind::File)
            for (const auto &source : layer.sources)
                if (participates(source) && source.admitted.contains(path))
                    value = source.admitted.value(path).value;
    return value;
}
std::optional<ConfigSource> ConfigLayers::source(const QString &layerId,
                                                 const QString &sourceId) const {
    for (const auto &layer : m_layers)
        if (layer.descriptor.id == layerId)
            for (const auto &source : layer.sources)
                if (source.source.id == sourceId)
                    return source.source;
    return std::nullopt;
}
QList<ConfigDiagnostic> ConfigLayers::diagnostics(const QString &layerId,
                                                  const QString &sourceId) const {
    for (const auto &layer : m_layers)
        if (layer.descriptor.id == layerId)
            for (const auto &source : layer.sources)
                if (source.source.id == sourceId)
                    return source.errors;
    return {};
}
QList<ConfigDiagnostic> ConfigLayers::rejectedDiagnostics() const {
    QList<ConfigDiagnostic> result;
    for (const auto &layer : m_layers)
        for (const auto &source : layer.sources)
            if (source.rejected && participates(source))
                result.append(source.errors);
    return result;
}

ConfigRoute ConfigLayers::route(const ConfigPath &path, const QString &layerId) const {
    for (const auto &layer : m_layers)
        if (layer.descriptor.id == layerId)
            for (const auto &source : layer.sources)
                if (startsWith(path, source.source.prefix))
                    return {layerId, source.source.id, source.source.prefix};
    return {};
}

ConfigInfo ConfigLayers::info(const ConfigPath &path) const {
    ConfigInfo out;
    out.intentRevision = m_intentRevision;
    const auto d = m_schema.descriptor(path);
    out.declared = valid() && d && d->type != ConfigDescriptor::Type::Group;
    if (!out.declared)
        return out;
    int winner = -1;
    int writable = -1;
    if (valueAt(m_schema.defaults(), path)) {
        out.contributions.append({"defaults", "schema", {}, true, true, true, true, {}, {}, 0});
        out.layerId = "defaults";
        out.sourceId = "schema";
        out.present = true;
    }
    for (qsizetype i = 0; i < m_layers.size(); ++i) {
        const auto &layer = m_layers[i];
        if (layer.descriptor.writable) {
            writable = i;
            out.route = route(path, layer.descriptor.id);
        }
        for (const auto &source : layer.sources) {
            if (!startsWith(path, source.source.prefix))
                continue;
            bool checked = true;
            for (const auto &error : source.errors)
                if (startsWith(path, error.path) || startsWith(error.path, path))
                    checked = false;
            const bool admitted = participates(source) && source.admitted.contains(path);
            out.contributions.append(
                {layer.descriptor.id,
                 source.source.id,
                 source.admitted.value(path).revision,
                 source.source.exists,
                 source.source.exists &&
                     bool(valueAt(source.source.raw, path.mid(source.source.prefix.size()))),
                 admitted,
                 checked,
                 {},
                 source.source.revision,
                 source.admitted.value(path).localIntentRevision});
            if (admitted) {
                out.layerId = layer.descriptor.id;
                out.sourceId = source.source.id;
                out.present = true;
                winner = i;
            }
        }
    }
    if (writable >= 0 && winner > writable)
        out.maskedBy = out.layerId;
    for (auto &c : out.contributions)
        if (c.admitted && (c.layerId != out.layerId || c.sourceId != out.sourceId))
            c.maskedBy = out.layerId;
    return out;
}

ConfigWriteResult ConfigLayers::write(const ConfigPath &path, const QVariant &value, bool runtime) {
    const auto previousLayers = m_layers;
    std::optional<ConfigLayers> before;
    if (!m_projections.isEmpty())
        before = *this;
    if (!valid())
        return {false, m_errors, {}, {}, m_intentRevision};
    const auto checked = m_schema.validate(path, value);
    if (!checked.ok())
        return {false, checked.errors, {}, {}, m_intentRevision};
    for (auto &layer : m_layers) {
        if (!(runtime ? layer.descriptor.kind == ConfigLayerKind::Runtime
                      : layer.descriptor.writable))
            continue;
        const auto target = route(path, layer.descriptor.id);
        if (!target.valid())
            return {false,
                    {m_schema.diagnostics().make(path, "no-write-route")},
                    {},
                    {},
                    m_intentRevision};
        for (auto &source : layer.sources)
            if (source.source.id == target.sourceId) {
                putValue(source.source.raw, path.mid(target.prefix.size()), checked.value);
                source.source.exists = true;
                source.source.localIntent = true;
                source.admitted.insert(path, {checked.value, {}, m_intentRevision + 1});
                for (auto it = source.errors.begin(); it != source.errors.end();)
                    if (startsWith(it->path, path) || startsWith(path, it->path))
                        it = source.errors.erase(it);
                    else
                        ++it;
                ++m_intentRevision; // Equal-valued assignment remains a new explicit choice.
                if (!m_deferProjections)
                    projectSources(&previousLayers);
                for (const auto &projectedLayer : m_layers)
                    for (const auto &state : projectedLayer.sources)
                        if (!m_deferProjections && participates(state) &&
                            !state.projectionError.isEmpty()) {
                            const auto errors = state.errors;
                            *this = std::move(*before);
                            return {false, errors, {}, {}, m_intentRevision};
                        }
                return {
                    true, {}, target, runtime ? QString() : info(path).maskedBy, m_intentRevision};
            }
    }
    return {
        false, {m_schema.diagnostics().make(path, "no-write-target")}, {}, {}, m_intentRevision};
}

ConfigWriteResult ConfigLayers::erase(const ConfigPath &path, const QString &layerId,
                                      bool runtime) {
    const auto before = *this;
    if (!valid())
        return {false, m_errors, {}, {}, m_intentRevision};
    if (!m_schema.descriptor(path))
        return {false,
                {m_schema.diagnostics().make(path, "undeclared-path")},
                {},
                {},
                m_intentRevision};
    for (auto &layer : m_layers) {
        if (layer.descriptor.id != layerId ||
            !(runtime ? layer.descriptor.kind == ConfigLayerKind::Runtime
                      : layer.descriptor.writable))
            continue;
        bool routed = false;
        for (auto &source : layer.sources) {
            const auto prefix = source.source.prefix;
            if (!startsWith(path, prefix) && !startsWith(prefix, path))
                continue;
            routed = true;
            source.source.localIntent = true;
            if (startsWith(prefix, path))
                source.source.raw.clear();
            else
                removeValue(source.source.raw, path.mid(prefix.size()));
            if (!runtime && source.source.pruneResetNamespaces)
                for (qsizetype n = path.size() - 1; n > prefix.size(); --n) {
                    const auto parent = path.mid(0, n);
                    const auto descriptor = m_schema.descriptor(parent);
                    const auto rawPath = parent.mid(prefix.size());
                    const auto raw = valueAt(source.source.raw, rawPath);
                    if (descriptor && descriptor->type == ConfigDescriptor::Type::Group && raw &&
                        raw->metaType().id() == QMetaType::QVariantMap && raw->toMap().isEmpty())
                        removeValue(source.source.raw, rawPath);
                }
            for (auto it = source.admitted.begin(); it != source.admitted.end();)
                if (startsWith(it.key(), path))
                    it = source.admitted.erase(it);
                else
                    ++it;
            for (auto it = source.errors.begin(); it != source.errors.end();)
                if (startsWith(it->path, path))
                    it = source.errors.erase(it);
                else
                    ++it;
        }
        if (!routed)
            return {false,
                    {m_schema.diagnostics().make(path, "no-write-route")},
                    {},
                    {},
                    m_intentRevision};
        ++m_intentRevision;
        if (!m_deferProjections)
            projectSources(&before.m_layers);
        for (const auto &projectedLayer : m_layers)
            for (const auto &state : projectedLayer.sources)
                if (!m_deferProjections && participates(state) &&
                    !state.projectionError.isEmpty()) {
                    const auto errors = state.errors;
                    *this = before;
                    return {false, errors, {}, {}, m_intentRevision};
                }
        return {true,
                {},
                route(path, layerId),
                runtime ? QString() : info(path).maskedBy,
                m_intentRevision};
    }
    return {
        false, {m_schema.diagnostics().make(path, "read-only-target")}, {}, {}, m_intentRevision};
}

ConfigWriteResult ConfigLayers::setUser(const ConfigPath &path, const QVariant &value) {
    return write(ownPath(path), value, false);
}
ConfigWriteResult ConfigLayers::applyEdits(const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    auto candidate = *this;
    candidate.m_deferProjections = true;
    QString writable;
    for (const auto &layer : m_layers)
        if (layer.descriptor.writable)
            writable = layer.descriptor.id;
    ConfigWriteResult result;
    for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
        result = it.value() ? candidate.setUser(it.key(), *it.value())
                            : candidate.reset(it.key(), writable);
        if (!result.accepted)
            return result;
    }
    candidate.m_deferProjections = false;
    candidate.projectSources(&m_layers);
    for (const auto &layer : candidate.m_layers)
        for (const auto &state : layer.sources)
            if (candidate.participates(state) && !state.projectionError.isEmpty())
                return {false, state.errors, {}, {}, m_intentRevision};
    *this = std::move(candidate);
    result.accepted = true;
    result.intentRevision = m_intentRevision;
    return result;
}
ConfigWriteResult ConfigLayers::setRuntime(const ConfigPath &path, const QVariant &value) {
    return write(ownPath(path), value, true);
}
ConfigWriteResult ConfigLayers::reset(const ConfigPath &path, const QString &targetLayer) {
    return erase(ownPath(path), targetLayer, false);
}
ConfigWriteResult ConfigLayers::clearRuntime(const ConfigPath &path) {
    for (const auto &layer : m_layers)
        if (layer.descriptor.kind == ConfigLayerKind::Runtime)
            return erase(ownPath(path), layer.descriptor.id, true);
    return {
        false, {m_schema.diagnostics().make(path, "no-runtime-target")}, {}, {}, m_intentRevision};
}
} // namespace configqml
