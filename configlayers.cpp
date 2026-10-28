// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlayers.h"
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
            m_errors.append(ConfigDiagnostic{{}, "layer-descriptor"});
        previous = kind;
        singletonKinds.insert(kind);
        ids.insert(layer.id);
        writable += layer.writable;
        LayerState state{layer, {}};
        state.descriptor.sources.clear(); // Only one owned raw copy, in SourceState.
        QSet<QString> sourceIds;
        for (const auto &source : layer.sources) {
            const auto d = schema.descriptor(source.prefix);
            if (source.id.isEmpty() || sourceIds.contains(source.id) || !d ||
                d->type != ConfigDescriptor::Type::Group)
                m_errors.append(ConfigDiagnostic{source.prefix, "source-descriptor"});
            for (const auto &other : state.sources)
                if (startsWith(source.prefix, other.source.prefix) ||
                    startsWith(other.source.prefix, source.prefix))
                    m_errors.append(ConfigDiagnostic{source.prefix, "overlapping-sources"});
            sourceIds.insert(source.id);
            SourceState owned;
            owned.source.id = source.id;
            owned.source.prefix = source.prefix;
            const auto result = admit(state, owned, source);
            // Schema-invalid input is retained as raw with diagnostics, not a broken store.
            if (!result.accepted && owned.errors.isEmpty())
                m_errors.append(result.errors);
            state.sources.append(owned);
        }
        m_layers.append(state);
    }
    if (writable > 1)
        m_errors.append(ConfigDiagnostic{{}, "multiple-writable-layers"});
}

ConfigOperation ConfigLayers::admit(LayerState &layer, SourceState &state,
                                    const ConfigSource &input) {
    const auto captured = ownValue(input.raw);
    if (!captured.ok())
        return {false, captured.errors};
    ConfigSource source = input;
    source.raw = captured.value.toMap();
    source.bytes = QByteArray(input.bytes.constData(), input.bytes.size());
    source.revision = QByteArray(input.revision.constData(), input.revision.size());
    QMap<ConfigPath, QVariant> admitted;
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
                        errors.append(ConfigDiagnostic{traversed, "namespace-type"});
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
                if (layer.descriptor.policy == ConfigAdmissionPolicy::RetainLastGood &&
                    invalidNamespace && state.admitted.contains(path))
                    admitted.insert(path, state.admitted.value(path));
                continue;
            }
            const auto checked = m_schema.validate(path, node);
            if (checked.ok())
                admitted.insert(path, checked.value);
            else {
                errors.append(checked.errors);
                if (layer.descriptor.policy == ConfigAdmissionPolicy::RetainLastGood &&
                    state.admitted.contains(path))
                    admitted.insert(path, state.admitted.value(path));
            }
        }
    }
    state.source = source;
    state.errors = errors;
    if (!errors.isEmpty() && layer.descriptor.policy == ConfigAdmissionPolicy::RejectInput)
        return {false, errors}; // Keep the prior admitted projection and the new raw evidence.
    state.admitted = admitted;
    return {true, errors};
}

ConfigOperation ConfigLayers::replaceSource(const QString &layerId, const ConfigSource &source) {
    if (!valid())
        return {false, m_errors};
    for (auto &layer : m_layers)
        if (layer.descriptor.id == layerId)
            for (auto &state : layer.sources)
                if (state.source.id == source.id) {
                    if (state.source.prefix != source.prefix)
                        return {false, {{source.prefix, "source-topology"}}};
                    return admit(layer, state, source);
                }
    return {false, {{{}, "unknown-source"}}};
}

QVariantMap ConfigLayers::effective() const {
    if (!valid())
        return {};
    auto out = m_schema.defaults();
    for (const auto &layer : m_layers)
        for (const auto &source : layer.sources)
            for (auto it = source.admitted.cbegin(); it != source.admitted.cend(); ++it)
                putValue(out, it.key(), it.value());
    return out;
}
std::optional<QVariant> ConfigLayers::value(const ConfigPath &path) const {
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
} // namespace configqml
