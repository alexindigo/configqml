// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configresult.h"
#include "configjsonbackend.h"
#include "configlistmodel.h"
#include "configrecordlistpolicy.h"
#include <QObject>
#include <QPointer>
#include <QThread>
#include <functional>
#include <memory>

namespace configqml {
class ConfigWorker;
class ConfigListModel;
class ConfigWorkerOwner;
struct ConfigWorkerPacket;
class ConfigStoreTestAccess;
class ConfigDraft;
class ConfigStoreHandle;
struct ConfigStoreOptions {
    ConfigSchema schema{ConfigDescriptor::group({})};
    QList<ConfigLayer> layers;
    QList<ConfigJsonFile> jsonFiles;
    bool autoSave = true;
    int autoSaveInterval = 300;
    ConfigJsonBackend::FaultHook fileFault;
    QString storeId;
    ConfigListModel::Numbers modelNumbers = ConfigListModel::Numbers::Native;
    QList<ConfigProjectionSpec> projections;
    QList<ConfigRecordListPolicySpec> recordListPolicies;
    QString evidenceDirectory;
    QList<ConfigPath> observedPaths;
    // Native, worker-only admission. Never pass an engine-bound callback here.
    std::function<QString(const ConfigInput &)> admission;
};
class ConfigStore : public QObject {
    Q_OBJECT
public:
    enum class Status { Loading, Ready, Error, Closing, Closed };
    Q_ENUM(Status)
    using Callback = std::function<void(const ConfigResult &)>;
    explicit ConfigStore(const ConfigStoreOptions &options, QObject *parent = nullptr);
    ~ConfigStore() override;
    Status status() const { return m_status; }
    QString error() const { return m_error; }
    QVariant value(const ConfigPath &path) const;
    QVariant presentation(const ConfigPath &path) const;
    ConfigPathState info(const ConfigPath &path) const;
    ConfigSnapshot snapshot() const {
        return m_admittedSnapshot ? *m_admittedSnapshot : ConfigSnapshot{};
    }
    QList<ConfigInput> inputs() const;
    QList<ConfigResult> failures() const { return {m_failures.cbegin(), m_failures.cend()}; }
    ConfigSchema schema() const { return m_schema; }
    ConfigListModel *listModel(const ConfigPath &path) const;
    ConfigListModel *keyedModel(const ConfigPath &path) const;
    bool canWriteUser(const ConfigPath &path) const;
    ConfigRequestHandle setUser(const ConfigPath &path, const QVariant &value,
                                QObject *context = nullptr, Callback callback = {},
                                ConfigDescriptor::Format format = ConfigDescriptor::Format::Native);
    ConfigRequestHandle setUser(const ConfigAdmission &value, QObject *context = nullptr,
                                Callback callback = {});
    ConfigRequestHandle setRuntime(const ConfigAdmission &value, QObject *context = nullptr,
                                   Callback callback = {});
    ConfigRequestHandle initializeIfMissing(const ConfigAdmission &value,
                                            QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle
    setRuntime(const ConfigPath &path, const QVariant &value, QObject *context = nullptr,
               Callback callback = {},
               ConfigDescriptor::Format format = ConfigDescriptor::Format::Native);
    ConfigRequestHandle reset(const ConfigPath &path, const QString &target,
                              QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle clearRuntime(const ConfigPath &path, QObject *context = nullptr,
                                     Callback callback = {});
    quint64 save(QObject *context = nullptr, Callback callback = {});
    quint64 reload(QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle
    initializeIfMissing(const ConfigPath &path, const QVariant &value, QObject *context = nullptr,
                        Callback callback = {},
                        ConfigDescriptor::Format format = ConfigDescriptor::Format::Native);
    quint64 flush(QObject *context = nullptr, Callback callback = {});
    quint64 close(QObject *context = nullptr, Callback callback = {});
    ConfigDraft *draft(const QList<ConfigPath> &paths = {}, QObject *parent = nullptr);
    ConfigStoreHandle *scope(const ConfigPath &prefix, bool writable = true,
                             QObject *parent = nullptr);
    quint64 exportProfile(const QString &name, const QString &file,
                          const QList<ConfigPath> &paths = {}, QObject *context = nullptr,
                          Callback callback = {});
    quint64 importProfile(const QString &file, const QList<ConfigPath> &paths = {},
                          QObject *context = nullptr, Callback callback = {});
    QList<ConfigSourceState> sourceStates() const { return m_sourceStates; }
    quint64 updateRecordListPolicy(const QString &policyId,
                                   const ConfigRecordListMetadata &metadata,
                                   QObject *context = nullptr, Callback callback = {});
    quint64 editRecordList(const QString &policyId, const ConfigRecordListEditOptions &options,
                           QObject *context = nullptr, Callback callback = {});
signals:
    void statusChanged();
    void valuesChanged(const QList<configqml::ConfigPath> &paths);
    void published(); // All eligible cache/model updates installed, including raw-only changes.
    void inputRejected(const configqml::ConfigPath &path, const QString &code);
    void completed(const configqml::ConfigResult &result);
    void autoSaved(const configqml::ConfigResult &result);
    void sourceError(const configqml::ConfigError &error);
    void sourceWarning(const configqml::ConfigError &warning);
    void migrationCompleted(const configqml::ConfigResult &result);
    void sourceStatesChanged();
    void sourceAccepted(const configqml::ConfigSourceAccepted &event);
    void projectionEffects(const configqml::ConfigProjectionEffectsEvent &event);

private:
    friend class ConfigWorker;
    friend class ConfigStoreTestAccess;
    friend class ConfigDraft;
    quint64 applyGroup(const QMap<ConfigPath, std::optional<QVariant>> &edits,
                       const QMap<ConfigPath, ConfigPathState> &fences, QObject *context,
                       Callback callback);
    quint64 profileRequest(const QString &operation, const QString &name, const QString &file,
                           const QList<ConfigPath> &paths, QObject *context, Callback callback);
    ConfigRequestHandle write(ConfigInput::Kind kind, const ConfigPath &path, const QVariant &value,
                              const QString &target, QObject *context, Callback callback,
                              ConfigDescriptor::Format format = ConfigDescriptor::Format::Native,
                              const ConfigAdmission *admission = nullptr);
    quint64 request(const QString &operation, QObject *context, Callback callback);
    quint64 registerRequest(QObject *context, Callback callback);
    void receive(const ConfigWorkerPacket &packet);
    void receiveResult(const ConfigResult &result);
    void deliver();
    void finishThread();
    struct Pending {
        QPointer<QObject> context;
        Callback callback;
    };
    ConfigWorker *m_worker = nullptr;
    std::shared_ptr<ConfigWorkerOwner> m_owner;
    std::optional<ConfigResult> m_closeResult;
    bool m_threadStopped = false;
    ConfigSchema m_schema;
    QMap<ConfigPath, ConfigRoute> m_userRoutes;
    QString m_userLayerId;
    QString m_storeId;
    ConfigSnapshot m_cache;
    QList<ConfigSourceState> m_sourceStates;
    QMap<QString, ConfigPath> m_policyPaths;
    mutable QMap<ConfigPath, QVariant> m_presentations;
    std::shared_ptr<const ConfigSnapshot> m_admittedSnapshot;
    Status m_status = Status::Loading;
    QString m_error;
    bool m_accepting = true;
    bool m_delivering = false;
    quint64 m_sequence = 0;
    quint64 m_nextCompletion = 1;
    QMap<quint64, Pending> m_pending;
    QMap<quint64, ConfigResult> m_results;
    QList<ConfigInput> m_inputs;
    QList<ConfigResult> m_failures;
    QMap<ConfigPath, std::shared_ptr<ConfigListModel>> m_models;
    bool m_publishingModels = false;
    qint64 m_worstPublicationNanos = 0;
    qint64 m_lastPlanningNanos = 0;
    qint64 m_lastLibraryPublicationNanos = 0;
};
} // namespace configqml
