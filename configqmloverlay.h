// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmlstore.h"

namespace configqml {
class ConfigQmlOverlay : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString editorId READ editorId CONSTANT)
    Q_PROPERTY(QObject *values READ values CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool conflicted READ conflicted NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(configqml::ConfigQmlStore::Status status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
public:
    ConfigQmlOverlay(ConfigStore *store, ConfigOverlayEditor *editor, QObject *parent);
    ~ConfigQmlOverlay() override;
    QString editorId() const { return m_adapter->editorId(); }
    QObject *values() const { return m_adapter->values(); }
    bool ready() const { return m_adapter->ready(); }
    bool dirty() const { return m_adapter->dirty(); }
    bool conflicted() const { return m_adapter->conflicted(); }
    bool busy() const { return m_adapter->busy(); }
    ConfigQmlStore::Status status() const { return m_adapter->status(); }
    QString error() const { return m_adapter->error(); }
    Q_INVOKABLE QString apply(const QJSValue &callback = {});
    Q_INVOKABLE QString resetOverrides(const QJSValue &callback = {});
    Q_INVOKABLE QString discard(const QJSValue &callback = {});
    Q_INVOKABLE bool unset(const QStringList &path);
    Q_INVOKABLE void detach();
signals:
    void stateChanged();
    void completed(const QVariantMap &result);
    void inputRejected(const QStringList &path, const QString &code);

private:
    ConfigQmlStore *m_adapter;
};
} // namespace configqml
