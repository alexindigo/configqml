// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configvalue.h"
#include <functional>

namespace configqml {
struct ConfigProjectionSource {
    QString sourceId, layerId;
    ConfigPath prefix;
    QString presence, revision;
};
struct ConfigProjectionInput {
    QVariantMap raw;
    ConfigProjectionSource source;
    int schemaVersion = 0;
    std::optional<int> priorVersion;
    QVariantMap defaults, lowerValues, values, compatibilityRaw, metadata;
};
struct ConfigProjectionEffect {
    QString effectId;
    quint64 version = 0;
    QString action;
    QVariantMap arguments;
};
struct ConfigProjectionDiagnostic {
    QString code;
    ConfigPath path;
    QString severity = "warning";
};
struct ConfigProjectionResult {
    QVariantMap values, compatibilityRaw;
    QList<ConfigProjectionEffect> effects;
    QList<ConfigProjectionDiagnostic> diagnostics;
};
struct ConfigProjectionSpec {
    QString projectionId;
    QStringList sourceIds;
    QVariantMap metadata;
    std::function<ConfigProjectionResult(const ConfigProjectionInput &)> transform;
};
struct ConfigProjectionEffectsEvent {
    QString runId, projectionId, sourceId, revision;
    QList<ConfigProjectionEffect> effects;
};
} // namespace configqml
