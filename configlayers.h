// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"

namespace configqml {
enum class ConfigLayerKind { File, Environment, Runtime };
enum class ConfigAdmissionPolicy { RejectInput, FieldFallback, RetainLastGood };

struct ConfigSource {
    QString id;
    ConfigPath prefix;
    QVariantMap raw; // Relative to prefix; unknown/invalid entries are retained.
    QByteArray bytes;
    QByteArray revision;
    bool exists = true;
};
struct ConfigLayer {
    QString id;
    ConfigLayerKind kind = ConfigLayerKind::File;
    ConfigAdmissionPolicy policy = ConfigAdmissionPolicy::FieldFallback;
    QList<ConfigSource> sources;
    bool writable = false;
};
struct ConfigOperation {
    bool accepted = false;
    QList<ConfigDiagnostic> errors;
};

// Synchronous, single-owner engine. M2 supplies the asynchronous store/worker.
class ConfigLayers {
public:
    ConfigLayers(const ConfigSchema &schema, const QList<ConfigLayer> &layers);
    bool valid() const { return m_errors.isEmpty(); }
    QList<ConfigDiagnostic> errors() const { return m_errors; }
    ConfigSchema schema() const { return m_schema; }
    QVariantMap effective() const;
    std::optional<QVariant> value(const ConfigPath &path) const;
    std::optional<ConfigSource> source(const QString &layerId, const QString &sourceId) const;
    QList<ConfigDiagnostic> diagnostics(const QString &layerId, const QString &sourceId) const;
    ConfigOperation replaceSource(const QString &layerId, const ConfigSource &source);

private:
    struct SourceState {
        ConfigSource source;
        QMap<ConfigPath, QVariant> admitted;
        QList<ConfigDiagnostic> errors;
    };
    struct LayerState {
        ConfigLayer descriptor;
        QList<SourceState> sources;
    };
    ConfigOperation admit(LayerState &layer, SourceState &state, const ConfigSource &source);
    ConfigSchema m_schema;
    QList<LayerState> m_layers;
    QList<ConfigDiagnostic> m_errors;
};
} // namespace configqml
