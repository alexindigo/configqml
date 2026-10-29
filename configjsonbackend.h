// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configjsoncodec.h"
#include <functional>
#include <QThread>

namespace configqml {
enum class ConfigWatchMode { Off, StartupSnapshot, Live };
enum class ConfigStartupErrorPolicy { FailReady, SkipProjectionRetainError };
struct ConfigJsonFile {
    QString layerId = "user";
    QString sourceId = "json";
    QString path; // Empty selects QStandardPaths::AppConfigLocation/config.json.
    bool optional = true;
    ConfigWatchMode watchMode = ConfigWatchMode::StartupSnapshot;
    ConfigStartupErrorPolicy startupErrorPolicy = ConfigStartupErrorPolicy::FailReady;
};
struct ConfigFileEdit {
    ConfigFileEdit() = default;
    ConfigFileEdit(ConfigPath path, std::optional<QVariant> value, bool ifMissing = false)
        : path(std::move(path)), value(std::move(value)), ifMissing(ifMissing) {}
    ConfigPath path;               // Relative to this file's explicit namespace prefix.
    std::optional<QVariant> value; // Missing means reset, null remains a value.
    bool ifMissing = false;
    std::optional<ConfigPath> fencePath;
    bool pruneEmpty = false; // Only declared namespace cleanup, never an atomic empty value.
};
struct ConfigJsonTransaction {
    ConfigBackendResult outcome;
    QVariantMap raw;
    bool published = false;
    QByteArray candidateBytes;
};
bool applyFileEdit(QVariantMap &raw, const ConfigFileEdit &edit);
class ConfigJsonBackend : public ConfigBackend {
public:
    using FaultHook = std::function<QString(const QString &phase, const QString &file)>;
    explicit ConfigJsonBackend(const QList<ConfigJsonFile> &files, FaultHook hook = {},
                               const QMap<QString, ConfigDiagnosticBuilder> &diagnostics = {});
    ConfigBackendCapabilities capabilities() const override;
    quint64 submit(const ConfigBackendRequest &request) override;
    std::optional<ConfigBackendResult> take(quint64 requestId) override;
    ConfigJsonTransaction reconcile(const QString &sourceId, const QVariantMap &baseline,
                                    const QList<ConfigFileEdit> &edits);
    ConfigError preserveBackup(const QString &sourceId, int version, const QByteArray &original);
    bool changed(const QString &sourceId) const;
    void confirm(const QString &sourceId, const ConfigRevision &revision);
    QString path(const QString &sourceId) const;
    QStringList watchPaths(const QString &sourceId) const;
    quint64 reads(const QString &sourceId) const { return m_reads.value(sourceId); }

private:
    struct Target {
        QString path;
        QStringList links;
        ConfigError error;
    };
    Target resolve(const QString &id) const;
    ConfigBackendResult read(const QString &id, const QString &expectedTarget = {});
    ConfigBackendResult publish(const ConfigBackendRequest &request, int directory,
                                const QString &target);
    ConfigError checkTarget(const QString &id, const QString &target, bool writing) const;
    QString fault(const QString &phase, const QString &target) const;
    QList<ConfigJsonFile> m_files;
    FaultHook m_hook;
    QMap<QString, ConfigDiagnosticBuilder> m_diagnostics;
    QMap<QString, QByteArray> m_confirmed;
    QMap<QString, QString> m_sourceTargets;
    QMap<QString, quint64> m_reads;
    QMap<quint64, ConfigBackendResult> m_results;
    quint64 m_next = 0;
    QThread *m_owner = QThread::currentThread();
};
} // namespace configqml
