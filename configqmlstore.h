// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmldeclaration.h"
#include "configstore.h"
#include "configdraft.h"
#include <QJSValue>
#include <QQmlParserStatus>
#include <QAbstractItemModel>

namespace configqml {
class ConfigQmlStore : public QObject, public QQmlParserStatus {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigStore)
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(configqml::ConfigQmlSchema *schema READ schema WRITE setSchema NOTIFY schemaChanged)
    Q_PROPERTY(QQmlListProperty<configqml::ConfigMemorySource> sources READ sources)
    Q_PROPERTY(QObject *values READ values NOTIFY valuesChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)
    Q_PROPERTY(bool autoSave MEMBER m_autoSave)
    Q_PROPERTY(int autoSaveInterval MEMBER m_autoSaveInterval)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftStateChanged)
    Q_PROPERTY(bool conflicted READ conflicted NOTIFY draftStateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY draftStateChanged)
public:
    enum Status { Loading, Ready, Error, Closing, Closed };
    Q_ENUM(Status)
    using QObject::QObject;
    void classBegin() override {}
    void componentComplete() override;
    ConfigQmlSchema *schema() const { return m_schema; }
    void setSchema(ConfigQmlSchema *schema);
    QQmlListProperty<ConfigMemorySource> sources() { return {this, &m_sources}; }
    QObject *values() const { return m_values; }
    Status status() const;
    bool ready() const { return status() == Ready; }
    QString error() const { return m_core ? m_core->error() : m_error; }
    ConfigStore *core() const { return m_core; }
    bool dirty() const { return m_draft && m_draft->dirty(); }
    bool conflicted() const { return m_draft && m_draft->conflicted(); }
    bool busy() const { return m_draft && m_draft->busy(); }
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
    Q_INVOKABLE QAbstractItemModel *listModel(const QStringList &path);
    Q_INVOKABLE QAbstractItemModel *keyedModel(const QStringList &path);
    Q_INVOKABLE QString save(const QJSValue &callback = {});
    Q_INVOKABLE QString reload(const QJSValue &callback = {});
    Q_INVOKABLE QString initializeIfMissing(const QStringList &path, const QVariant &value,
                                            const QJSValue &callback = {});
    Q_INVOKABLE QString flush(const QJSValue &callback = {});
    Q_INVOKABLE QString close(const QJSValue &callback = {});
    Q_INVOKABLE ConfigQmlStore *scope(const QStringList &key, bool writable = true);
    Q_INVOKABLE ConfigQmlStore *draft(const QVariantList &paths = {});
    Q_INVOKABLE QString apply(const QJSValue &callback = {});
    Q_INVOKABLE bool discard();
    Q_INVOKABLE void detach();
    Q_INVOKABLE QString exportProfile(const QString &name, const QString &file,
                                      const QVariantList &paths = {},
                                      const QJSValue &callback = {});
    Q_INVOKABLE QString importProfile(const QString &file, const QVariantList &paths = {},
                                      const QJSValue &callback = {});
signals:
    void schemaChanged();
    void valuesChanged();
    void statusChanged();
    void completed(const QVariantMap &result);
    void autoSaved(const QVariantMap &result);
    void sourceError(const QVariantMap &error);
    void inputRejected(const QStringList &path, const QString &code);
    void settingValuesChanged(const QList<configqml::ConfigPath> &paths);
    void draftStateChanged();
    void migrationCompleted(const QVariantMap &result);

private:
    ConfigStore::Callback callback(const QJSValue &function);
    void reject(const ConfigPath &path, const QString &code);
    void raise(const QString &code);
    QList<ConfigPath> selectedPaths(const QVariantList &paths, bool *ok);
    void initializeHandle(ConfigStore *core, const ConfigPath &prefix, bool writable,
                          ConfigDraft *draft = nullptr);
    QPointer<ConfigQmlSchema> m_schema;
    QList<ConfigMemorySource *> m_sources;
    QPointer<ConfigStore> m_core;
    QPointer<QObject> m_values;
    QString m_error;
    bool m_complete = false;
    bool m_autoSave = true;
    QMap<ConfigPath, QPointer<QAbstractItemModel>> m_models;
    int m_autoSaveInterval = 300;
    ConfigPath m_prefix;
    QPointer<ConfigDraft> m_draft;
    bool m_writable = true;
    bool m_isHandle = false;
    ConfigDiagnosticBuilder m_diagnostics;
};
QObject *createConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor,
                               const ConfigPath &path, QObject *parent);
} // namespace configqml
