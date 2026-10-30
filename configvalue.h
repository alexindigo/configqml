// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <QStringList>
#include <QVariant>
#include <optional>
#include <memory>

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
struct ConfigDescriptor;
struct ConfigError;
struct ConfigResult;
// The sole diagnostic construction/redaction boundary. Context-free value tools
// have no sensitive schema; store/schema adapters always bind immutable metadata.
class ConfigDiagnosticBuilder {
public:
    ConfigDiagnosticBuilder() = default;
    explicit ConfigDiagnosticBuilder(const ConfigDescriptor &root, ConfigPath base = {});
    ConfigDiagnosticBuilder at(const ConfigPath &origin) const;
    ConfigPath path(const ConfigPath &path) const;
    ConfigDiagnostic make(const ConfigPath &path, const QString &code) const;
    ConfigValueResult sanitize(ConfigValueResult result) const;
    ConfigError sanitize(ConfigError error) const;
    ConfigResult sanitize(ConfigResult result) const;
    ConfigError error(const ConfigDiagnostic &diagnostic, const QString &source,
                      const QString &operation, const QString &file = {}, qsizetype line = 0) const;

private:
    std::shared_ptr<const ConfigDescriptor> m_root;
    ConfigPath m_base;
    ConfigPath m_origin;
};

// Invalid QVariant is missing/undefined, nullptr is explicit null.
ConfigValueResult ownValue(const QVariant &input, const ConfigPath &path = {},
                           const ConfigDiagnosticBuilder &diagnostics = {});
ConfigPath ownPath(const ConfigPath &path);
QString ownString(const QString &value);
bool isNull(const QVariant &value);
bool startsWith(const ConfigPath &path, const ConfigPath &prefix);
std::optional<QVariant> valueAt(const QVariantMap &tree, const ConfigPath &path);
void putValue(QVariantMap &tree, const ConfigPath &path, const QVariant &value);
void removeValue(QVariantMap &tree, const ConfigPath &path);
} // namespace configqml
