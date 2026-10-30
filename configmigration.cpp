// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "confignumber.h"

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
            if (!result.coveredRevisions.isEmpty())
                result.publicationState = "partial";
        } else
            result.status = planned.isEmpty() ? "nothing_to_save" : "saved";
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
        if (m_files.contains(updated.id)) {
            const auto transaction = m_json->reconcile(updated.id, migration.original.raw,
                                                       {{{}, migration.transformed, false}});
            const auto &outcome = transaction.outcome;
            const QString state =
                outcome.publicationState == ConfigBackendResult::PublicationState::Known
                    ? "known"
                    : "uncertain";
            result.publications.append(
                {updated.id, outcome.success, outcome.revision, state, outcome.error});
            if (!outcome.success) {
                result.publicationState = state;
                return finish(outcome.error.code, updated.id);
            }
            updated.raw = transaction.raw;
            updated.bytes = outcome.bytes;
            updated.revision = outcome.revision.token;
            m_baselines[updated.id] = transaction.raw;
            m_revisions[updated.id] = outcome.revision;
            result.coveredRevisions[updated.id] = outcome.revision;
        } else {
            // The memory backend's seed below is its initial confirmed publication.
            updated.bytes = encodeMemoryValue(updated.raw);
        }
        for (auto &layer : m_options.layers)
            if (layer.id == migration.layer)
                for (auto &source : layer.sources)
                    if (source.id == updated.id)
                        source = updated;
    }
    return finish();
}
} // namespace configqml
