// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "confignumber.h"
#include <QCryptographicHash>

namespace configqml {
bool ConfigWorker::migrateSources() {
    if (!m_options.schema.valid() || !m_initializationError.isEmpty() ||
        m_options.schema.version() == 0)
        return m_initializationError.isEmpty();
    struct Migration {
        QString layer;
        ConfigSource original;
        QVariantMap transformed;
        int fromVersion;
    };
    QList<Migration> planned;
    ConfigResult result;
    result.storeId = m_options.storeId;
    result.operation = "migration";
    const auto finish = [&](const QString &code = QString(), const QString &id = QString()) {
        if (!code.isEmpty()) {
            m_initializationError = code;
            result.status = "failed";
            if (result.error.empty())
                result.error = {code, id, "migration", m_json->path(id)};
        } else
            result.status = planned.isEmpty() ? "nothing_to_save" : "saved";
        finalizePublicationState(result);
        ConfigWorkerPacket packet;
        packet.migrationResult = result;
        post(packet);
        return code.isEmpty();
    };
    // Transform every source first: an exception or failed transform cannot alter originals.
    for (const auto &layer : m_options.layers) {
        if (layer.kind != ConfigLayerKind::File)
            continue;
        for (const auto &source : layer.sources) {
            if (!source.exists)
                continue;
            int version = 0;
            if (source.raw.contains("_configqmlVersion")) {
                const auto checked = ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)
                                         .validate(source.raw.value("_configqmlVersion"));
                if (!checked.ok() || checked.value.toLongLong() < 0 ||
                    checked.value.toLongLong() > m_options.schema.version())
                    return finish("migration-version", source.id);
                version = checked.value.toInt();
            }
            if (version == m_options.schema.version())
                continue;
            if (!layer.writable)
                return finish("migration-read-only", source.id);
            auto raw = source.raw;
            raw.remove("_configqmlVersion");
            for (int current = version; current < m_options.schema.version(); ++current) {
                std::function<ConfigValueResult(const QVariantMap &)> transform;
                for (const auto &migration : m_options.schema.migrations())
                    if (migration.fromVersion == current)
                        transform = migration.transform;
                if (!transform)
                    return finish("migration-missing-step", source.id);
                ConfigValueResult transformed;
                try {
                    transformed = transform(raw);
                } catch (...) {
                    return finish("migration-exception", source.id);
                }
                if (!transformed.ok() ||
                    transformed.value.metaType().id() != QMetaType::QVariantMap)
                    return finish("migration-transform", source.id);
                const auto captured =
                    ownValue(transformed.value, source.prefix, m_options.schema.diagnostics());
                if (!captured.ok())
                    return finish("migration-value", source.id);
                raw = captured.value.toMap();
                raw.remove("_configqmlVersion");
            }
            raw["_configqmlVersion"] = m_options.schema.version();
            planned.append({layer.id, source, raw, version});
        }
    }
    // Backups retain exact original bytes and never overwrite conflicting evidence.
    for (const auto &migration : planned) {
        if (!m_files.contains(migration.original.id))
            continue;
        result.error = m_json->preserveBackup(migration.original.id, migration.fromVersion,
                                              migration.original.bytes);
        if (!result.error.empty()) {
            return finish(result.error.code, migration.original.id);
        }
    }
    for (const auto &migration : planned) {
        ConfigSource updated = migration.original;
        updated.raw = migration.transformed;
        QString retentionError;
        if (m_files.contains(updated.id)) {
            const auto capacity = prepareEvidence(updated.id, EvidencePublicationReserve, &result);
            if (!capacity.isEmpty())
                return finish(capacity, updated.id);
            const auto transaction = m_json->reconcile(
                updated.id, migration.original.raw, {{{}, migration.transformed, false}},
                ConfigSourcePreimage{m_revisions.value(updated.id), migration.original.bytes},
                canonicalizer(updated.id, {}, &result));
            if (transaction.published)
                m_unknownWarnings.append(transaction.warnings);
            for (const auto &observation : transaction.observations) {
                const auto error =
                    retainObservation(updated.id, observation.result, observation.phase,
                                      observation.role, {}, &result);
                if (!error.isEmpty())
                    retentionError = error;
            }
            const auto &outcome = transaction.outcome;
            recordPublication(result, updated.id, outcome);
            if (!retentionError.isEmpty())
                result.error = {retentionError, updated.id, "migration", m_json->path(updated.id)};
            if (!outcome.success) {
                return finish(retentionError.isEmpty() ? outcome.error.code : retentionError,
                              updated.id);
            }
            updated.raw = transaction.raw;
            updated.bytes = outcome.bytes;
            updated.revision = outcome.revision.token;
            m_baselines[updated.id] = transaction.raw;
            m_revisions[updated.id] = outcome.revision;
            m_sourceBytes[updated.id] = outcome.bytes;
        } else {
            // The memory backend's seed below is its initial confirmed publication.
            updated.raw = canonicalMemory(updated.id, updated.raw, true);
            updated.bytes = encodeMemoryValue(updated.raw);
        }
        for (auto &layer : m_options.layers)
            if (layer.id == migration.layer)
                for (auto &source : layer.sources)
                    if (source.id == updated.id)
                        source = updated;
        if (!retentionError.isEmpty())
            return finish(retentionError, updated.id);
    }
    return finish();
}
ConfigJsonBackend::Canonicalizer
ConfigWorker::canonicalizer(const QString &id, const std::optional<ConfigCommand> &command,
                            ConfigResult *result) {
    if (!m_strictSources.contains(id))
        return {};
    return [this, id, command, result](QVariantMap &raw, const ConfigBackendResult &original,
                                       QList<ConfigError> &pendingWarnings) -> ConfigError {
        const auto prefix = m_strictSources.value(id);
        const auto descriptor = *m_options.schema.descriptor(prefix);
        const auto closed = closeConfigValue(descriptor, raw, prefix, true);
        ConfigJsonCodec codec(m_options.schema.diagnostics().at(prefix));
        const auto disk =
            original.revision.exists ? codec.decode(original.bytes, id).raw : QVariantMap{};
        const auto diskClosure = closeConfigValue(descriptor, disk, prefix, true);
        QList<ConfigEvidenceReference> evidence;
        if (!diskClosure.removed.isEmpty()) {
            const auto observation = QCryptographicHash::hash(
                encodeMemoryValue(QVariantMap{
                    {"sourceId", id},
                    {"revision", original.revision.token},
                    {"bytesDigest",
                     QCryptographicHash::hash(original.bytes, QCryptographicHash::Sha256)}}),
                QCryptographicHash::Sha256);
            if (!m_unknownEvidence.contains(observation)) {
                ConfigResult retained;
                const auto error = retainObservation(id, original, "unknown-data", "unknown-data",
                                                     command, &retained);
                if (!error.isEmpty())
                    return {error, id, "unknown-data", m_json->path(id)};
                m_unknownEvidence.insert(observation, retained.evidence);
            }
            evidence = m_unknownEvidence.value(observation);
            if (result)
                result->evidence.append(evidence);
        }
        const auto baseline = closeConfigValue(descriptor, m_baselines.value(id), prefix, true);
        QList<ConfigPath> warnings;
        for (const auto &path : diskClosure.removed)
            if (!baseline.removed.contains(path))
                warnings.append(path);
        // Migration output can itself introduce unknown members, not present on disk.
        for (const auto &path : closed.removed)
            if ((!command || !baseline.removed.contains(path)) && !warnings.contains(path))
                warnings.append(path);
        const auto warn = [&](const ConfigPath &path, const QString &code) {
            auto warning = m_options.schema.diagnostics().error(
                m_options.schema.diagnostics().make(path, code), id, "unknown-data",
                m_json->path(id));
            warning.evidence = evidence;
            pendingWarnings.append(warning);
        };
        for (qsizetype i = 0; i < qMin(qsizetype(64), warnings.size()); ++i)
            warn(warnings[i], "unknown-data-removed");
        if (warnings.size() > 64)
            warn(prefix, "unknown-data-truncated");
        raw = closed.value.toMap();
        return {};
    };
}
QVariantMap ConfigWorker::canonicalMemory(const QString &id, const QVariantMap &raw,
                                          bool migration) {
    if (!m_strictSources.contains(id))
        return raw;
    const auto prefix = m_strictSources.value(id);
    const auto closed = closeConfigValue(*m_options.schema.descriptor(prefix), raw, prefix, true);
    if (migration) {
        for (qsizetype i = 0; i < qMin(qsizetype(64), closed.removed.size()); ++i)
            m_unknownWarnings.append(m_options.schema.diagnostics().error(
                m_options.schema.diagnostics().make(closed.removed[i], "unknown-data-removed"), id,
                "unknown-data"));
        if (closed.removed.size() > 64)
            m_unknownWarnings.append(m_options.schema.diagnostics().error(
                m_options.schema.diagnostics().make(prefix, "unknown-data-truncated"), id,
                "unknown-data"));
    }
    return closed.value.toMap();
}
} // namespace configqml
