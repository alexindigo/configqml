// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
// Internal protocol. No private Qt headers, no QML objects or engine callbacks.
#include "configstore.h"
#include "configmemorybackend.h"
#include "configprojection_p.h"
#include <QQueue>
#include <QTimer>
#include <QFileSystemWatcher>
#include <memory>

namespace configqml {
QByteArray encodeMemoryValue(const QVariantMap &raw);
QVariantMap decodeMemoryValue(const QByteArray &bytes);
struct ConfigWorkerPacket {
    ConfigSnapshot snapshot;
    std::shared_ptr<const ConfigSnapshot> admitted;
    bool initial = false;
    bool terminal = false;
    QString error;
    std::optional<ConfigResult> result;
    bool autoSaveRequested = false;
    std::optional<ConfigError> sourceError;
    std::optional<ConfigResult> migrationResult;
    std::optional<QList<ConfigSourceState>> sourceStates;
    std::optional<ConfigSourceAccepted> sourceAccepted;
    QList<ConfigProjectionEffectsEvent> projectionEffects;
    QList<ConfigError> projectionDiagnostics;
    QMap<QString, ConfigGroupInfo> groups;
    QMap<QString, quint64> groupGenerations;
    QMap<ConfigPath, QMap<quint64, ConfigModelPlan>> models;
    qint64 planningNanos = 0;
};
struct ConfigCommand {
    ConfigCommand() = default;
    ConfigCommand(quint64 requestId, quint64 prefix, QString operation,
                  std::optional<ConfigInput> input)
        : requestId(requestId), prefix(prefix), operation(std::move(operation)),
          input(std::move(input)) {}
    quint64 requestId = 0;
    quint64 prefix = 0;
    QString operation;
    std::optional<ConfigInput> input;
    QString file;
    QString profileName;
    QList<ConfigPath> selection;
    QString policyId;
    std::optional<ConfigRecordListMetadata> policyMetadata;
    std::optional<ConfigRecordListEditOptions> listEdits;
    QString conversionError;
    std::optional<ConfigRetainedGroupOptions> retainedGroup;
    bool retainedLookup = false;
    QString groupToken, groupState;
    // Object-thread snapshot, including earlier pending lifecycle claims.
    QMap<QString, quint64> groupGenerations;
};
struct ConfigPendingIntent {
    quint64 id = 0, input = 0;
    ConfigPath path;
    ConfigFileEdit edit;
    QString reservation;
};
struct ConfigRetainedCapture {
    ConfigGroupInfo info;
    quint64 generation = 0;
    QByteArray commandBody;
    QList<ConfigPath> selections;
    QMap<QString, QSet<quint64>> intents;
    QMap<QString, QVariantMap> bases;
    QMap<QString, ConfigRevision> revisions;
    QMap<QString, ConfigRevision> uncertain;
    QMap<QString, QByteArray> uncertainBytes;
    QMap<QString, QVariantMap> originalBases;
    QMap<QString, ConfigRevision> originalRevisions;
    QList<ConfigResult> receipts;
    QMap<QString, ConfigRevision> confirmedRevisions;
};
class ConfigWorker : public QObject {
    Q_OBJECT
public:
    explicit ConfigWorker(ConfigStoreOptions options, QString initializationError);
    void initialize();
    void enqueue(const ConfigCommand &command);
    void abandon();
    void acknowledgeModels(const QMap<ConfigPath, quint64> &generations);
    // Internal deterministic scheduling, accessible only through the test friend.
    void holdInputs(bool hold);
    void releaseInputs(int count);
    void holdReplies(bool hold);
    void releaseReplies(int count);
    void releaseBackend();
    void scheduleBackend(ConfigMemoryBackend::Schedule schedule);
    void fail(const QString &code);
    ConfigMemoryBackend *backend() const { return m_backend.get(); }
    ConfigLayers *layers() const { return m_layers.get(); }
    ConfigJsonBackend *jsonBackend() const { return m_json.get(); }
    void setAdmission(std::function<QString(const ConfigInput &)> admission) {
        m_options.admission = std::move(admission);
    }
signals:
    void packetReady(const configqml::ConfigWorkerPacket &packet);

private:
    void drain();
    void process(const ConfigCommand &command);
    void coverInput(const ConfigCommand &command);
    void acceptIntent(const ConfigInput &input);
    ConfigWorkerPacket publish(const std::optional<ConfigResult> &result = {},
                               bool initial = false);
    void post(const ConfigWorkerPacket &packet);
    ConfigResult resultFor(const ConfigCommand &command) const;
    bool inheritFailures(ConfigResult &result) const;
    void beginSave(const ConfigCommand &command);
    void continueSave();
    void finishSave();
    bool reloadFiles(const std::optional<ConfigCommand> &command = {});
    void armWatches();
    void scheduleAutoSave();
    void processGroup(const ConfigCommand &command);
    void profile(const ConfigCommand &command);
    bool migrateSources();
    void recordListCommand(const ConfigCommand &command);
    QString quarantined(const ConfigPath &path) const;
    QString observeSource(const QString &id, const ConfigBackendResult &read,
                          const ConfigError &error, bool startup);
    void retainedGroupCommand(const ConfigCommand &command);
    void recordPersistent(const ConfigInput &input, const ConfigLayers &before);
    void recordPersistentEdit(const ConfigRoute &route, const ConfigPath &path,
                              const std::optional<QVariant> &value, bool ifMissing, quint64 input,
                              const ConfigLayers &before);
    QList<ConfigFileEdit> pendingEdits(const QString &source) const;
    QList<ConfigPendingIntent> eligibleIntents(const QString &source,
                                               const ConfigCommand &command) const;
    bool reserved(const ConfigPath &path) const;
    QStringList unresolvedGroups(quint64 prefix) const;
    void confirmed(const QString &id, const QVariantMap &raw, const ConfigBackendResult &outcome);
    void refreshDirty(const QString &id);
    void describeGroup(ConfigResult &result, const ConfigGroupInfo &info) const;
    bool recoverOwnPublication(const QString &source, const ConfigBackendResult &read);
    ConfigStoreOptions m_options;
    QString m_initializationError;
    std::unique_ptr<ConfigLayers> m_layers;
    std::unique_ptr<ConfigLayers> m_diskLayers;
    QList<ConfigSourceState> m_sourceStates;
    QMap<QString, ConfigRecordListPolicySpec> m_listPolicies;
    QMap<QString, ConfigRecordListMetadata> m_listMetadata;
    QMap<QString, quint64> m_listRevisions;
    QMap<ConfigPath, QVariant> m_listInputs, m_listOutputs;
    QMap<ConfigPath, QStringList> m_listMetadataKeys;
    QString m_acceptOrigin, m_acceptCause;
    quint64 m_sourceEventId = 0;
    QMap<ConfigPath, QVariantList> m_diskFacts;
    quint64 m_observationOrder = 0;
    std::unique_ptr<ConfigMemoryBackend> m_backend;
    std::unique_ptr<ConfigJsonBackend> m_json;
    QMap<QString, ConfigJsonFile> m_files;
    QMap<QString, ConfigRoute> m_fileRoutes;
    QMap<QString, QVariantMap> m_baselines;
    QMap<QString, QList<ConfigPendingIntent>> m_intents;
    quint64 m_nextIntent = 0;
    QMap<QString, ConfigRetainedCapture> m_groups;
    QMap<QString, QString> m_groupKeys;
    QMap<QString, ConfigRevision> m_pendingRevisions;
    QMap<QString, QList<ConfigPendingIntent>> m_saveIntents;
    QMap<QString, QVariantMap> m_saveBases;
    QMap<QString, ConfigRevision> m_saveRevisions;
    QVariantMap m_activeRaw;
    bool m_unresolvedSave = false;
    bool m_recoveryRead = false;
    QSet<QString> m_recoveryChecked, m_recoveryWhole;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_watchTimer = nullptr;
    QTimer *m_autoTimer = nullptr;
    QQueue<ConfigCommand> m_queue;
    QQueue<ConfigWorkerPacket> m_replies;
    bool m_holdInputs = false;
    bool m_holdReplies = false;
    bool m_busy = false;
    bool m_failed = false;
    bool m_closed = false;
    bool m_draining = false;
    quint64 m_prefix = 0;
    quint64 m_generation = 0;
    QMap<ConfigPath, quint64> m_pathGenerations;
    QMap<ConfigPath, quint64> m_pathInputs;
    QMap<ConfigPath, quint64> m_pathIntents;
    QMap<ConfigPath, QVariantList> m_content;
    QMap<ConfigPath, quint64> m_contentRevisions;
    ConfigSnapshot m_previous;
    QMap<ConfigPath, QMap<quint64, QList<ConfigModelRow>>> m_modelVersions;
    QMap<QString, ConfigRoute> m_routes;
    QMap<QString, ConfigRevision> m_revisions;
    QMap<QString, quint64> m_dirty;
    QList<ConfigResult> m_failures;
    ConfigCommand m_save;
    ConfigResult m_saveResult;
    bool m_saveRetainedFailure = false;
    QStringList m_saveSources;
    QString m_activeSource;
    QByteArray m_activeBytes;
    quint64 m_activeBackend = 0;
};
} // namespace configqml
