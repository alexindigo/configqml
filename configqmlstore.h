// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmldeclaration.h"
#include "configstore.h"
#include "configdraft.h"
#include "configoverlayeditor.h"
#include <QJSValue>
#include <QQmlParserStatus>
#include <QAbstractItemModel>
#include <QProperty>

namespace configqml {
class ConfigQmlOverlay;
class ConfigQmlStore : public QObject, public QQmlParserStatus {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigStore)
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(configqml::ConfigQmlSchema *schema READ schema WRITE setSchema NOTIFY schemaChanged)
    Q_PROPERTY(QQmlListProperty<configqml::ConfigMemorySource> sources READ sources)
    Q_PROPERTY(QObject *values READ values NOTIFY valuesChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged BINDABLE bindableStatus)
    Q_PROPERTY(bool ready READ ready NOTIFY statusChanged BINDABLE bindableReady)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged BINDABLE bindableError)
    Q_PROPERTY(bool autoSave MEMBER m_autoSave)
    Q_PROPERTY(int autoSaveInterval MEMBER m_autoSaveInterval)
    Q_PROPERTY(qint64 profileMaxBytes MEMBER m_profileMaxBytes)
    Q_PROPERTY(QQmlListProperty<configqml::ConfigQmlProjection> projections READ projections)
    Q_PROPERTY(QQmlListProperty<configqml::ConfigQmlRecordListPolicy> recordListPolicies READ
                   recordListPolicies)
    Q_PROPERTY(QString evidenceDirectory MEMBER m_evidenceDirectory)
    Q_PROPERTY(QVariantList observedPaths MEMBER m_observedPaths)
    Q_PROPERTY(QVariantList sourceStates READ sourceStates NOTIFY sourceStatesChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftStateChanged BINDABLE bindableDirty)
    Q_PROPERTY(bool conflicted READ conflicted NOTIFY draftStateChanged BINDABLE bindableConflicted)
    Q_PROPERTY(bool busy READ busy NOTIFY draftStateChanged BINDABLE bindableBusy)
    Q_PROPERTY(QString editorId READ editorId CONSTANT)
    Q_PROPERTY(QObject *preview READ preview CONSTANT)
public:
    enum Status { Loading, Ready, Error, Closing, Closed };
    Q_ENUM(Status)
    explicit ConfigQmlStore(QObject *parent = nullptr);
    void classBegin() override {}
    void componentComplete() override;
    ConfigQmlSchema *schema() const { return m_schema; }
    void setSchema(ConfigQmlSchema *schema);
    QQmlListProperty<ConfigMemorySource> sources() { return {this, &m_sources}; }
    QQmlListProperty<ConfigQmlProjection> projections() { return {this, &m_projections}; }
    QQmlListProperty<ConfigQmlRecordListPolicy> recordListPolicies() {
        return {this, &m_recordListPolicies};
    }
    QVariantList sourceStates() const;
    QObject *values() const { return m_values; }
    Status status() const;
    bool ready() const { return status() == Ready; }
    QString error() const {
        return m_overlay ? m_overlay->error() : m_core ? m_core->error() : m_error;
    }
    ConfigStore *core() const { return m_core; }
    bool dirty() const { return m_overlay ? m_overlay->dirty() : m_draft && m_draft->dirty(); }
    bool conflicted() const {
        return m_overlay ? m_overlay->conflicted() : m_draft && m_draft->conflicted();
    }
    bool busy() const { return m_overlay ? m_overlay->busy() : m_draft && m_draft->busy(); }
    QBindable<Status> bindableStatus() const { return QBindable<Status>(&m_publishedStatus); }
    QBindable<bool> bindableReady() const { return QBindable<bool>(&m_publishedReady); }
    QBindable<QString> bindableError() const { return QBindable<QString>(&m_publishedError); }
    QBindable<bool> bindableDirty() const { return QBindable<bool>(&m_publishedDirty); }
    QBindable<bool> bindableConflicted() const { return QBindable<bool>(&m_publishedConflicted); }
    QBindable<bool> bindableBusy() const { return QBindable<bool>(&m_publishedBusy); }
    QString editorId() const { return m_overlay ? m_overlay->editorId() : QString(); }
    bool canWrite(const ConfigPath &path) const;
    QVariant read(const ConfigPath &path) const;
    ConfigRequestHandle write(const ConfigPath &path, const QVariant &value,
                              const QJSValue &callback = {});
    Q_INVOKABLE QString setUser(const QStringList &path, const QVariant &value,
                                const QJSValue &callback = {});
    Q_INVOKABLE QString setRuntime(const QStringList &path, const QVariant &value,
                                   const QJSValue &callback = {});
    Q_INVOKABLE QString reset(const QStringList &path, const QString &target,
                              const QJSValue &callback = {});
    Q_INVOKABLE QString clearRuntime(const QStringList &path, const QJSValue &callback = {});
    Q_INVOKABLE QVariantMap info(const QStringList &path) const;
    Q_INVOKABLE configqml::ConfigListModel *listModel(const QStringList &path);
    Q_INVOKABLE configqml::ConfigListModel *keyedModel(const QStringList &path);
    Q_INVOKABLE QString save(const QJSValue &callback = {});
    Q_INVOKABLE QString reload(const QJSValue &callback = {});
    Q_INVOKABLE QString initializeIfMissing(const QStringList &path, const QVariant &value,
                                            const QJSValue &callback = {});
    Q_INVOKABLE QString flush(const QJSValue &callback = {});
    Q_INVOKABLE QString close(const QJSValue &callback = {});
    Q_INVOKABLE ConfigQmlStore *scope(const QStringList &key, bool writable = true);
    Q_INVOKABLE ConfigQmlStore *draft(const QVariantList &paths = {});
    Q_INVOKABLE QJSValue copyValue(const QJSValue &value);
    QObject *preview();
    Q_INVOKABLE ConfigQmlStore *previewScope(const QStringList &path);
    Q_INVOKABLE QString apply(const QJSValue &callback = {});
    Q_INVOKABLE bool discard();
    Q_INVOKABLE QString openOverlay(const QJSValue &options, const QJSValue &callback = {});
    Q_INVOKABLE QString resetOverrides(const QJSValue &callback = {});
    Q_INVOKABLE bool unset(const QStringList &path);
    Q_INVOKABLE void detach();
    Q_INVOKABLE QString exportProfile(const QString &name, const QString &file,
                                      const QVariantList &paths = {},
                                      const QJSValue &callback = {});
    Q_INVOKABLE QString importProfile(const QString &file, const QVariantList &paths = {},
                                      const QJSValue &callback = {});
    Q_INVOKABLE QString updateRecordListPolicy(const QString &policyId, const QJSValue &metadata,
                                               const QJSValue &callback = {});
    Q_INVOKABLE QString editRecordList(const QString &policyId, const QJSValue &options,
                                       const QJSValue &callback = {});
    Q_INVOKABLE QString retainGroup(const QJSValue &options, const QJSValue &callback = {});
    Q_INVOKABLE QString saveGroup(const QString &token, const QJSValue &callback = {});
    Q_INVOKABLE QString retryGroup(const QString &token, const QJSValue &callback = {});
    Q_INVOKABLE QString releaseGroup(const QString &token, const QJSValue &callback = {});
    Q_INVOKABLE QVariantMap groupInfo(const QString &token) const;
signals:
    void schemaChanged();
    void valuesChanged();
    void statusChanged();
    void completed(const QVariantMap &result);
    void autoSaved(const QVariantMap &result);
    void sourceError(const QVariantMap &error);
    void sourceWarning(const QVariantMap &warning);
    void inputRejected(const QStringList &path, const QString &code);
    void settingValuesChanged(const QList<configqml::ConfigPath> &paths);
    void draftStateChanged();
    void migrationCompleted(const QVariantMap &result);
    void sourceStatesChanged();
    void sourceAccepted(const QVariantMap &event);
    void projectionEffects(const QVariantMap &event);

private:
    friend class ConfigQmlOverlay;
    ConfigStore::Callback callback(const QJSValue &function);
    QJSEngine *scriptEngine() const;
    void reject(const ConfigPath &path, const QString &code);
    void raise(const QString &code);
    void initializeCompatibilitySignals();
    void initializeOverlaySignals();
    void initializeNotifications();
    void registerNotifications(ConfigChangePublisher *publisher);
    void prepareState();
    QVariantMap resultMap(const ConfigResult &result) const;
    QList<ConfigPath> selectedPaths(const QVariantList &paths, bool *ok);
    void initializeHandle(ConfigStore *core, const ConfigPath &prefix, bool writable,
                          ConfigDraft *draft = nullptr, bool preview = false);
    QPointer<ConfigQmlSchema> m_schema;
    QList<ConfigMemorySource *> m_sources;
    QList<ConfigQmlProjection *> m_projections;
    QList<ConfigQmlRecordListPolicy *> m_recordListPolicies;
    QMap<QString, ConfigDescriptor> m_policyRecords;
    QString m_evidenceDirectory;
    QVariantList m_observedPaths;
    QPointer<ConfigStore> m_core;
    QPointer<QObject> m_values;
    QString m_error;
    bool m_complete = false;
    bool m_autoSave = true;
    QMap<ConfigPath, QPointer<ConfigListModel>> m_models;
    int m_autoSaveInterval = 300;
    qint64 m_profileMaxBytes = 16 * 1024 * 1024;
    ConfigPath m_prefix;
    QPointer<ConfigDraft> m_draft;
    QPointer<ConfigDraft> m_previewDraft;
    QPointer<ConfigQmlStore> m_previewAdapter;
    bool m_isPreview = false;
    QPointer<ConfigOverlayEditor> m_overlay;
    QPointer<QObject> m_callbackContext;
    QPointer<ConfigStoreHandle> m_nativeScope;
    QMap<QString, QPointer<QObject>> m_overlayHandles;
    QSet<quint64> m_overlayRequests;
    bool m_writable = true;
    bool m_isHandle = false;
    ConfigDiagnosticBuilder m_diagnostics;
    mutable QMap<ConfigPath, QVariant> m_scriptPresentations;
    QProperty<Status> m_publishedStatus{Loading};
    QProperty<bool> m_publishedReady{false}, m_publishedDirty{false}, m_publishedConflicted{false},
        m_publishedBusy{false};
    QProperty<QString> m_publishedError;
};
QObject *createConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor,
                               const ConfigPath &path, QObject *parent);
void notifyConfigNamespace(QObject *object, const QList<ConfigPath> &paths);
void prepareConfigNamespace(QObject *object, const QList<ConfigPath> &paths);
} // namespace configqml
