// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"

namespace configqml {
struct ConfigEnvironmentMapping {
    QString name;
    ConfigPath path;
};
struct ConfigEnvironmentResult {
    QVariantMap values;
    QList<ConfigDiagnostic> errors;
    bool ok() const { return errors.isEmpty(); }
};
// Pure adapter: no qgetenv/process-global state. Parsing uses the declared schema type.
ConfigEnvironmentResult mapEnvironment(const ConfigSchema &schema,
                                       const QList<ConfigEnvironmentMapping> &mappings,
                                       const QMap<QString, QString> &environment);
// Convenience only for lower-case ASCII simple names; opaque/camelCase paths need explicit aliases.
std::optional<QList<ConfigEnvironmentMapping>> environmentMappings(const ConfigSchema &schema,
                                                                   const QString &prefix,
                                                                   const QList<ConfigPath> &paths);
} // namespace configqml
