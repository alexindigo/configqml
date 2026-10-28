// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmldeclaration.h"
#include "configstore.h"
#include <QJSValue>
#include <QQmlParserStatus>

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
    Q_INVOKABLE QString save(const QJSValue &callback = {});
    Q_INVOKABLE QString flush(const QJSValue &callback = {});
    Q_INVOKABLE QString close(const QJSValue &callback = {});
signals:
    void schemaChanged();
    void valuesChanged();
    void statusChanged();
    void completed(const QVariantMap &result);
    void inputRejected(const QStringList &path, const QString &code);

private:
    ConfigStore::Callback callback(const QJSValue &function);
    void reject(const ConfigPath &path, const QString &code);
    QPointer<ConfigQmlSchema> m_schema;
    QList<ConfigMemorySource *> m_sources;
    QPointer<ConfigStore> m_core;
    QPointer<QObject> m_values;
    QString m_error;
    bool m_complete = false;
};
QObject *createConfigNamespace(ConfigQmlStore *store, const ConfigDescriptor &descriptor,
                               const ConfigPath &path, QObject *parent);
} // namespace configqml
