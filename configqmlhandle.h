// SPDX-License-Identifier: LGPL-3.0-or-later
// Thin QML adapter over a native generated handle. Bindings follow the native
// publication owner; every verb returns the shared pending reply.
#pragma once
#include "confighandle.h"
#include "configqmlpendingreply.h"
#include <QJSValue>
#include <QHash>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace configqml {
class ConfigQmlHandle : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool valid READ valid NOTIFY stateChanged)
    Q_PROPERTY(QString health READ health NOTIFY stateChanged)
    Q_PROPERTY(QString kind READ kind CONSTANT)
    Q_PROPERTY(QStringList path READ path CONSTANT)
    Q_PROPERTY(bool isField READ isField CONSTANT)
    Q_PROPERTY(QStringList capabilities READ capabilities NOTIFY stateChanged)
    Q_PROPERTY(QJSValue descriptor READ descriptor CONSTANT)
    Q_PROPERTY(QVariant value READ value NOTIFY valueChanged)
    Q_PROPERTY(bool present READ present NOTIFY valueChanged)
    Q_PROPERTY(QJSValue values READ values CONSTANT)
    Q_PROPERTY(QJSValue state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariant generation READ generation NOTIFY stateChanged)
    Q_PROPERTY(QJSValue scopes READ scopes CONSTANT)
    Q_PROPERTY(QJSValue fields READ fields CONSTANT)
    Q_PROPERTY(configqml::ConfigQmlHandle *preview READ preview CONSTANT)
public:
    // Owner-held roots are C++-owned children of their QML owner; an opened
    // edit root is JS-owned and owns its session. Lookups are C++-owned
    // children of their root wrapper.
    static ConfigQmlHandle *owned(ConfigHandle *handle, QJSEngine *engine, QObject *owner,
                                  const QString &name);
    static ConfigQmlHandle *adopt(ConfigHandle *handle, QJSEngine *engine);
    ConfigHandle *native() const { return m_handle; }
    bool valid() const;
    QString health() const;
    QString kind() const;
    QStringList path() const;
    bool isField() const;
    QStringList capabilities() const;
    QJSValue descriptor() const;
    QVariant value() const;
    bool present() const;
    QJSValue values();
    QJSValue state() const;
    QVariant generation() const;
    QJSValue scopes();
    QJSValue fields();
    ConfigQmlHandle *preview();
    Q_INVOKABLE configqml::ConfigQmlHandle *scope(const QStringList &path);
    Q_INVOKABLE configqml::ConfigQmlHandle *select(const QVariantList &paths);
    Q_INVOKABLE configqml::ConfigQmlPendingReply *read(const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *set(const QJSValue &value,
                                                      const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *remove(const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *unset(const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *apply(const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *discard(const QJSValue &options = {});
    // Tokens arrive as the library-owned values replies hand out (QVariant).
    Q_INVOKABLE configqml::ConfigQmlPendingReply *observe(const QVariant &after = {},
                                                          const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *whenCurrent(const QVariant &after = {},
                                                              const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *edit(const QJSValue &options = {});
    Q_INVOKABLE configqml::ConfigQmlPendingReply *abandon(const QJSValue &options = {});
signals:
    void valueChanged();
    void stateChanged();

private:
    ConfigQmlHandle(ConfigHandle *handle, QJSEngine *engine, QObject *parent);
    ConfigQmlHandle *wrap(ConfigHandle *handle);
    ConfigQmlPendingReply *reply(ConfigPendingReply *native) const;
    ConfigQmlPendingReply *refuse(const QString &code, const QString &phase) const;
    using Verb = ConfigPendingReply *(ConfigHandle::*)(const ConfigHandleOptions &);
    ConfigQmlPendingReply *invoke(const QJSValue &options, Verb verb,
                                  const QJSValue *value = nullptr, const QVariant *after = nullptr);
    QJSValue tree(const QString &kind);
    QPointer<ConfigHandle> m_handle;
    QPointer<QJSEngine> m_engine;
    QPointer<ConfigQmlHandle> m_root;
    QHash<ConfigHandle *, QPointer<ConfigQmlHandle>> m_wrappers; // Root only.
    mutable QJSValue m_descriptor;
};
} // namespace configqml
