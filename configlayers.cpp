// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlayers.h"
#include "configjsoncodec.h"
#include <QSet>

namespace configqml {
ConfigLayers::ConfigLayers(const ConfigSchema &schema, const QList<ConfigLayer> &layers)
    : m_schema(schema), m_errors(schema.errors()) {
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
        bool rejected = false;
        for (const auto &source : state.sources)
            rejected |= source.rejected;
        if (rejected)
            for (auto &source : state.sources)
                source.admitted.clear();
        m_layers.append(state);
    }
    if (writable > 1)
        m_errors.append(m_schema.diagnostics().make({}, "multiple-writable-layers"));
}
bool ConfigLayers::rejects(const LayerState &layer, const ConfigPath &path) const {
    const auto policy = m_schema.policy(path);
    return policy == ConfigDescriptor::AdmissionPolicy::Strict ||
           (policy == ConfigDescriptor::AdmissionPolicy::Inherit &&
            (layer.descriptor.policy == ConfigAdmissionPolicy::RejectInput ||
             layer.descriptor.shippedDefaults));
}

ConfigOperation ConfigLayers::admit(LayerState &layer, SourceState &state,
                                    const ConfigSource &input) {
    const auto captured = ownValue(input.raw, input.prefix, m_schema.diagnostics());
    if (!captured.ok())
        return {false, captured.errors};
    ConfigSource source = input;
    source.id = QString(input.id.constData(), input.id.size());
    source.prefix = ownPath(input.prefix);
    source.raw = captured.value.toMap();
    source.bytes = QByteArray(input.bytes.constData(), input.bytes.size());
    source.revision = QByteArray(input.revision.constData(), input.revision.size());
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
    if (!valid())
        return {false, m_errors};
    for (auto &layer : m_layers) {
        if (layer.descriptor.id != layerId)
            continue;
        auto candidate = layer;
        ConfigOperation result{true, {}};
        QSet<QString> seen;
        for (const auto &source : sources) {
            bool found = false;
            if (seen.contains(source.id))
                return {false, {m_schema.diagnostics().make(source.prefix, "duplicate-source")}};
            seen.insert(source.id);
            for (auto &state : candidate.sources)
                if (state.source.id == source.id) {
                    found = true;
                    if (state.source.prefix != source.prefix)
                        return {false,
                                {m_schema.diagnostics().make(source.prefix, "source-topology")}};
                    const auto admitted = admit(candidate, state, source);
                    result.accepted &= admitted.accepted;
                    result.errors.append(admitted.errors);
                }
            if (!found)
                return {false, {m_schema.diagnostics().make({}, "unknown-source")}};
        }
        if (!result.accepted)
            for (int i = 0; i < candidate.sources.size(); ++i)
                candidate.sources[i].admitted = layer.sources[i].admitted;
        layer = std::move(candidate); // Raw evidence survives strict layer rejection.
        return result;
    }
    return {false, {m_schema.diagnostics().make({}, "unknown-source")}};
}

QVariantMap ConfigLayers::effective() const {
    if (!valid())
        return {};
    auto out = m_schema.defaults();
    for (const auto &layer : m_layers)
        for (const auto &source : layer.sources)
            for (auto it = source.admitted.cbegin(); it != source.admitted.cend(); ++it)
                putValue(out, it.key(), it.value().value);
    return out;
}
std::optional<QVariant> ConfigLayers::value(const ConfigPath &path) const {
    if (!valid() || !m_schema.descriptor(path))
        return std::nullopt;
    return valueAt(effective(), path);
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
            if (source.rejected)
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
            const bool admitted = source.admitted.contains(path);
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
                source.admitted.insert(path, {checked.value, {}, m_intentRevision + 1});
                for (auto it = source.errors.begin(); it != source.errors.end();)
                    if (startsWith(it->path, path) || startsWith(path, it->path))
                        it = source.errors.erase(it);
                    else
                        ++it;
                ++m_intentRevision; // Equal-valued assignment remains a new explicit choice.
                return {
                    true, {}, target, runtime ? QString() : info(path).maskedBy, m_intentRevision};
            }
    }
    return {
        false, {m_schema.diagnostics().make(path, "no-write-target")}, {}, {}, m_intentRevision};
}

ConfigWriteResult ConfigLayers::erase(const ConfigPath &path, const QString &layerId,
                                      bool runtime) {
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
            if (startsWith(prefix, path))
                source.source.raw.clear();
            else
                removeValue(source.source.raw, path.mid(prefix.size()));
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
