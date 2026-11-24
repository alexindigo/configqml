// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmlhandle.h"
#include "configqmlreadprojection.h"
#include "configqmlstore.h"

namespace configqml {
class ConfigQmlOverlay;
class ConfigQmlPrimitives;

class ConfigQmlStorePrimitives : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(configqml::ConfigQmlHandle *root READ root NOTIFY rootsChanged)
    Q_PROPERTY(configqml::ConfigQmlHandle *runtimeRoot READ runtimeRoot NOTIFY rootsChanged)
    Q_PROPERTY(configqml::ConfigQmlReadProjection *readProjection READ readProjection CONSTANT)
public:
    ConfigQmlHandle *root() { return m_owner->root(); }
    ConfigQmlHandle *runtimeRoot() { return m_owner->runtimeRoot(); }
    ConfigQmlReadProjection *readProjection() { return m_owner->readProjection(); }
    Q_INVOKABLE QString abandonApply(const QJSValue &callback = {}) {
        return m_owner->abandonApply(callback);
    }

signals:
    void rootsChanged();

private:
    friend class ConfigQmlPrimitives;
    explicit ConfigQmlStorePrimitives(ConfigQmlStore *owner);
    ConfigQmlStore *const m_owner;
};

class ConfigQmlOverlayPrimitives : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(configqml::ConfigQmlHandle *root READ root CONSTANT)
    Q_PROPERTY(configqml::ConfigQmlReadProjection *readProjection READ readProjection CONSTANT)
public:
    ConfigQmlHandle *root();
    ConfigQmlReadProjection *readProjection();

private:
    friend class ConfigQmlPrimitives;
    explicit ConfigQmlOverlayPrimitives(ConfigQmlOverlay *owner);
    ConfigQmlOverlay *const m_owner;
};

// Keep new QML members off legacy lexical scopes. The owner, not the
// singleton or the JS engine, owns the returned adapter.
class ConfigQmlPrimitives : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigPrimitives)
    QML_SINGLETON
public:
    Q_INVOKABLE configqml::ConfigQmlStorePrimitives *forStore(configqml::ConfigQmlStore *store);
    Q_INVOKABLE configqml::ConfigQmlOverlayPrimitives *forOverlay(QObject *overlay);
};
} // namespace configqml
