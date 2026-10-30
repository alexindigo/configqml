// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configvalue.h"

namespace configqml {
// Owned JSON decimal syntax, before any binary floating-point conversion.
// Keep this value in raw layers; schema admission chooses the numeric domain.
class ConfigNumber {
public:
    ConfigNumber() = default;
    static std::optional<ConfigNumber> fromJson(const QString &token);
    QString token() const { return m_token; }
    ConfigValueResult integer(bool unsignedType, const ConfigPath &path = {},
                              const ConfigDiagnosticBuilder &diagnostics = {}) const;
    ConfigValueResult real(const ConfigPath &path = {},
                           const ConfigDiagnosticBuilder &diagnostics = {}) const;
    bool operator==(const ConfigNumber &other) const { return m_token == other.m_token; }

private:
    QString m_token;
};

struct ConfigJsonEncodeResult {
    QByteArray bytes;
    QList<ConfigDiagnostic> errors;
    bool ok() const { return errors.isEmpty(); }
};
// Shared value boundary for structured inputs and codecs. Numeric tokens are
// never passed through QJsonValue/double. This is not a storage backend.
struct ConfigJsonLayout {
    QMap<ConfigPath, QStringList> keys;
    qsizetype errorOffset = 0;
};
ConfigValueResult decodeJsonValue(const QByteArray &bytes, const ConfigPath &path = {},
                                  ConfigJsonLayout *layout = nullptr,
                                  const ConfigDiagnosticBuilder &diagnostics = {});
ConfigJsonEncodeResult encodeJsonValue(const QVariant &value, const ConfigPath &path = {},
                                       const ConfigDiagnosticBuilder &diagnostics = {});
} // namespace configqml
Q_DECLARE_METATYPE(configqml::ConfigNumber)
