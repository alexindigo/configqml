// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configstore.h"
#include <QObject>

namespace configqml {
// Base for application-authored Q_PROPERTY classes. Properties forward through
// these methods; presentation caching stays with the store, without another
// admission, value-resolution or persistence owner.
class ConfigFacade : public QObject {
    Q_OBJECT
public:
    ConfigFacade(ConfigStore *store, ConfigPath prefix = {}, QObject *parent = nullptr);
signals:
    void settingChanged(const configqml::ConfigPath &relativePath);
    void inputRejected(const configqml::ConfigPath &relativePath, const QString &code);

protected:
    QVariant readSetting(const ConfigPath &relativePath) const;
    ConfigRequestHandle writeSetting(const ConfigPath &relativePath, const QVariant &qmlValue);

private:
    QPointer<ConfigStore> m_store;
    ConfigPath m_prefix;
    ConfigDiagnosticBuilder m_diagnostics;
};
} // namespace configqml
