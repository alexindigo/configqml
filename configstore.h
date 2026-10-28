// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configresult.h"
#include "configjsonbackend.h"
#include <QObject>
#include <QPointer>
#include <QThread>
#include <functional>
#include <memory>

namespace configqml {
class ConfigWorker;
class ConfigWorkerOwner;
struct ConfigWorkerPacket;
class ConfigStoreTestAccess;
struct ConfigStoreOptions {
    ConfigSchema schema{ConfigDescriptor::group({})};
    QList<ConfigLayer> layers;
    QList<ConfigJsonFile> jsonFiles;
    bool autoSave = true;
    int autoSaveInterval = 300;
    ConfigJsonBackend::FaultHook fileFault;
    QString storeId;
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
    ConfigPathState info(const ConfigPath &path) const;
    ConfigSnapshot snapshot() const {
        return m_admittedSnapshot ? *m_admittedSnapshot : ConfigSnapshot{};
    }
    QList<ConfigInput> inputs() const { return {m_inputs.cbegin(), m_inputs.cend()}; }
    QList<ConfigResult> failures() const { return {m_failures.cbegin(), m_failures.cend()}; }
    ConfigSchema schema() const { return m_schema; }
    bool canWriteUser(const ConfigPath &path) const;
    ConfigRequestHandle setUser(const ConfigPath &path, const QVariant &value,
                                QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle setRuntime(const ConfigPath &path, const QVariant &value,
                                   QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle reset(const ConfigPath &path, const QString &target,
                              QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle clearRuntime(const ConfigPath &path, QObject *context = nullptr,
                                     Callback callback = {});
    quint64 save(QObject *context = nullptr, Callback callback = {});
    quint64 reload(QObject *context = nullptr, Callback callback = {});
    ConfigRequestHandle initializeIfMissing(const ConfigPath &path, const QVariant &value,
                                            QObject *context = nullptr, Callback callback = {});
    quint64 flush(QObject *context = nullptr, Callback callback = {});
    quint64 close(QObject *context = nullptr, Callback callback = {});
signals:
    void statusChanged();
    void valuesChanged(const QList<configqml::ConfigPath> &paths);
    void inputRejected(const configqml::ConfigPath &path, const QString &code);
    void completed(const configqml::ConfigResult &result);
    void autoSaved(const configqml::ConfigResult &result);
    void sourceError(const configqml::ConfigError &error);

private:
    friend class ConfigWorker;
    friend class ConfigStoreTestAccess;
    ConfigRequestHandle write(ConfigInput::Kind kind, const ConfigPath &path, const QVariant &value,
                              const QString &target, QObject *context, Callback callback);
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
    qint64 m_worstPublicationNanos = 0;
};
} // namespace configqml
