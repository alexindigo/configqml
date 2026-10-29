// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <QStringList>
#include <QVariant>
#include <optional>

namespace configqml {
const char *version();
using ConfigPath = QStringList;

struct ConfigDiagnostic {
    ConfigPath path;
    QString code; // Never includes input values.
};

struct ConfigValueResult {
    QVariant value;
    QList<ConfigDiagnostic> errors;
    bool ok() const { return errors.isEmpty(); }
};

// Invalid QVariant is missing/undefined, nullptr is explicit null.
ConfigValueResult ownValue(const QVariant &input, const ConfigPath &path = {});
ConfigPath ownPath(const ConfigPath &path);
QString ownString(const QString &value);
bool isNull(const QVariant &value);
bool startsWith(const ConfigPath &path, const ConfigPath &prefix);
std::optional<QVariant> valueAt(const QVariantMap &tree, const ConfigPath &path);
void putValue(QVariantMap &tree, const ConfigPath &path, const QVariant &value);
void removeValue(QVariantMap &tree, const ConfigPath &path);
} // namespace configqml
