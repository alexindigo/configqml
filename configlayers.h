// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include "configprojection.h"
#include <QSet>

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
    bool json = false; // Raw JSON URL strings are decoded only for admitted projections.
    QString fallbackFor;
    bool available = true;
    std::optional<bool> diskExists;
    bool localIntent = false;
};
struct ConfigLayer {
    QString id;
    ConfigLayerKind kind = ConfigLayerKind::File;
    ConfigAdmissionPolicy policy = ConfigAdmissionPolicy::FieldFallback;
    QList<ConfigSource> sources;
    bool writable = false;
    bool shippedDefaults = false;
};
struct ConfigOperation {
    bool accepted = false;
    QList<ConfigDiagnostic> errors;
};

struct ConfigRoute {
    QString layerId;
    QString sourceId;
    ConfigPath prefix;
    bool valid() const { return !sourceId.isEmpty(); }
};
struct ConfigContribution {
    QString layerId;
    QString sourceId;
    QByteArray revision;
    bool sourceExists = false;
    bool rawPresent = false;
    bool admitted = false;
    bool valid = true;
    QString maskedBy;
    QByteArray rawRevision;
    quint64 localIntentRevision = 0;
    bool operator==(const ConfigContribution &o) const {
        return layerId == o.layerId && sourceId == o.sourceId && revision == o.revision &&
               sourceExists == o.sourceExists && rawPresent == o.rawPresent &&
               admitted == o.admitted && valid == o.valid && maskedBy == o.maskedBy &&
               rawRevision == o.rawRevision && localIntentRevision == o.localIntentRevision;
    }
};
struct ConfigInfo {
    bool declared = false;
    bool present = false;
    QString layerId;
    QString sourceId;
    QString maskedBy; // Mask of ordinary writable-user intent, even if no override exists yet.
    ConfigRoute route;
    QList<ConfigContribution> contributions;
    quint64 intentRevision = 0;
};
struct ConfigWriteResult {
    bool accepted = false;
    QList<ConfigDiagnostic> errors;
    ConfigRoute route;
    QString maskedBy;
    quint64 intentRevision = 0;
    QString description() const {
        return maskedBy.isEmpty() ? QString() : QStringLiteral("masked by ") + maskedBy;
    }
};

// Synchronous, single-owner engine. M2 supplies the asynchronous store/worker.
class ConfigLayers {
public:
    ConfigLayers(const ConfigSchema &schema, const QList<ConfigLayer> &layers,
                 const QList<ConfigProjectionSpec> &projections = {});
    bool valid() const { return m_errors.isEmpty(); }
    QList<ConfigDiagnostic> errors() const { return m_errors; }
    ConfigSchema schema() const { return m_schema; }
    QVariantMap effective() const;
    std::optional<QVariant> value(const ConfigPath &path) const;
    std::optional<QVariant> unmaskedValue(const ConfigPath &path) const;
    std::optional<ConfigSource> source(const QString &layerId, const QString &sourceId) const;
    QList<ConfigDiagnostic> diagnostics(const QString &layerId, const QString &sourceId) const;
    QList<ConfigDiagnostic> rejectedDiagnostics() const;
    ConfigOperation replaceSource(const QString &layerId, const ConfigSource &source);
    ConfigOperation replaceSources(const QString &layerId, const QList<ConfigSource> &sources);
    ConfigOperation replaceAllSources(const QMap<QString, QList<ConfigSource>> &sources);
    ConfigWriteResult applyEdits(const QMap<ConfigPath, std::optional<QVariant>> &edits);
    ConfigInfo info(const ConfigPath &path) const;
    ConfigWriteResult setUser(const ConfigPath &path, const QVariant &value);
    ConfigWriteResult reset(const ConfigPath &path, const QString &targetLayer);
    ConfigWriteResult setRuntime(const ConfigPath &path, const QVariant &value);
    ConfigWriteResult clearRuntime(const ConfigPath &path);
    QString fallbackState(const QString &sourceId) const;
    QList<ConfigProjectionEffectsEvent> takeProjectionEffects();
    QList<QPair<QString, ConfigDiagnostic>> takeProjectionDiagnostics();

private:
    friend class ConfigWorker;
    void removeTransientLayers();
    struct AdmittedValue {
        QVariant value;
        QByteArray revision;
        quint64 localIntentRevision = 0;
    };
    struct SourceState {
        ConfigSource source;
        QMap<ConfigPath, AdmittedValue> admitted;
        QList<ConfigDiagnostic> errors;
        bool rejected = false;
        QString projectionError;
    };
    struct LayerState {
        ConfigLayer descriptor;
        QList<SourceState> sources;
    };
    ConfigOperation admit(LayerState &layer, SourceState &state, const ConfigSource &source,
                          bool projected = false);
    bool rejects(const LayerState &layer, const ConfigPath &path) const;
    ConfigRoute route(const ConfigPath &path, const QString &layerId) const;
    ConfigWriteResult write(const ConfigPath &path, const QVariant &value, bool runtime);
    ConfigWriteResult erase(const ConfigPath &path, const QString &layerId, bool runtime);
    bool participates(const SourceState &source) const;
    void projectSources(const QList<LayerState> *previous = nullptr);
    QList<ConfigProjectionSpec> m_projections;
    QList<ConfigProjectionEffectsEvent> m_effects;
    QList<QPair<QString, ConfigDiagnostic>> m_projectionDiagnostics;
    QSet<QString> m_effectRuns;
    bool m_deferProjections = false;
    ConfigSchema m_schema;
    QList<LayerState> m_layers;
    QList<ConfigDiagnostic> m_errors;
    quint64 m_intentRevision = 0;
};
} // namespace configqml
