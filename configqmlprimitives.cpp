// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlprimitives.h"
#include "configqmloverlay.h"
#include <QQmlEngine>

namespace configqml {
ConfigQmlStorePrimitives::ConfigQmlStorePrimitives(ConfigQmlStore *owner)
    : QObject(owner), m_owner(owner) {
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    connect(owner, &ConfigQmlStore::statusChanged, this, &ConfigQmlStorePrimitives::rootsChanged);
}

ConfigQmlOverlayPrimitives::ConfigQmlOverlayPrimitives(ConfigQmlOverlay *owner)
    : QObject(owner), m_owner(owner) {
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
}

ConfigQmlHandle *ConfigQmlOverlayPrimitives::root() { return m_owner->root(); }
ConfigQmlReadProjection *ConfigQmlOverlayPrimitives::readProjection() {
    return m_owner->readProjection();
}

ConfigQmlStorePrimitives *ConfigQmlPrimitives::forStore(ConfigQmlStore *store) {
    if (!store)
        return nullptr;
    auto *view = store->findChild<ConfigQmlStorePrimitives *>(Qt::FindDirectChildrenOnly);
    return view ? view : new ConfigQmlStorePrimitives(store);
}

ConfigQmlOverlayPrimitives *ConfigQmlPrimitives::forOverlay(QObject *overlay) {
    auto *owner = qobject_cast<ConfigQmlOverlay *>(overlay);
    if (!owner)
        return nullptr;
    auto *view = owner->findChild<ConfigQmlOverlayPrimitives *>(Qt::FindDirectChildrenOnly);
    return view ? view : new ConfigQmlOverlayPrimitives(owner);
}
} // namespace configqml
