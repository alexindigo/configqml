// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmlstore.h"

namespace configqml {
class ConfigQmlOverlay : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString editorId READ editorId CONSTANT)
    Q_PROPERTY(QString placementToken READ placementToken NOTIFY stateChanged BINDABLE
                   bindablePlacementToken)
    Q_PROPERTY(QObject *values READ values CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged BINDABLE bindableReady)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged BINDABLE bindableDirty)
    Q_PROPERTY(bool conflicted READ conflicted NOTIFY stateChanged BINDABLE bindableConflicted)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged BINDABLE bindableBusy)
    Q_PROPERTY(configqml::ConfigQmlStore::Status status READ status NOTIFY stateChanged BINDABLE
                   bindableStatus)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged BINDABLE bindableError)
public:
    ConfigQmlOverlay(ConfigStore *store, ConfigOverlayEditor *editor, QObject *parent);
    ~ConfigQmlOverlay() override;
    QString editorId() const { return m_adapter->editorId(); }
    QString placementToken() const {
        return m_adapter->m_overlay ? m_adapter->m_overlay->placementToken() : QString();
    }
    QObject *values() const { return m_adapter->values(); }
    bool ready() const { return m_adapter->ready(); }
    bool dirty() const { return m_adapter->dirty(); }
    bool conflicted() const { return m_adapter->conflicted(); }
    bool busy() const { return m_adapter->busy(); }
    ConfigQmlStore::Status status() const { return m_adapter->status(); }
    QString error() const { return m_adapter->error(); }
    QBindable<ConfigQmlStore::Status> bindableStatus() const { return m_adapter->bindableStatus(); }
    QBindable<bool> bindableReady() const { return m_adapter->bindableReady(); }
    QBindable<QString> bindableError() const { return m_adapter->bindableError(); }
    QBindable<bool> bindableDirty() const { return m_adapter->bindableDirty(); }
    QBindable<bool> bindableConflicted() const { return m_adapter->bindableConflicted(); }
    QBindable<bool> bindableBusy() const { return m_adapter->bindableBusy(); }
    QBindable<QString> bindablePlacementToken() const {
        return QBindable<QString>(&m_publishedPlacementToken);
    }
    Q_INVOKABLE QString apply(const QJSValue &callback = {});
    Q_INVOKABLE QString resetOverrides(const QJSValue &callback = {});
    Q_INVOKABLE QString discard(const QJSValue &callback = {});
    Q_INVOKABLE QString save(const QJSValue &callback = {});
    Q_INVOKABLE QString flush(const QJSValue &callback = {});
    Q_INVOKABLE bool unset(const QStringList &path);
    Q_INVOKABLE void detach();
signals:
    void stateChanged();
    void completed(const QVariantMap &result);
    void inputRejected(const QStringList &path, const QString &code);

private:
    ConfigQmlStore *m_adapter;
    QProperty<QString> m_publishedPlacementToken;
};
} // namespace configqml
