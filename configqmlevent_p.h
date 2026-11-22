// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmlvalue.h"
#include "configobservation.h"
#include <functional>

namespace configqml {
// The loader owns only immutable native event data, never the emitting store.
QJSValue qmlEvent(QJSEngine *engine, const QStringList &keys,
                  std::function<QVariant(const QString &)> loader);
QJSValue qmlResultEvent(QJSEngine *engine, const ConfigResult &result,
                        const ConfigDiagnosticBuilder &diagnostics = {});
QJSValue qmlResultEvent(QJSEngine *engine, const ConfigResult &result,
                        const ConfigDiagnosticBuilder &diagnostics,
                        const ConfigObservation &observation);
QJSValue qmlErrorEvent(QJSEngine *engine, const ConfigError &error);
QJSValue qmlSelectedStateEvent(QJSEngine *engine, const ConfigSelectedState &state,
                               const std::optional<ConfigDescriptor> &descriptor);
QJSValue qmlReadSnapshotEvent(QJSEngine *engine, const ConfigReadSnapshot &snapshot);
QJSValue qmlReadResultEvent(QJSEngine *engine, const ConfigReadResult &result);
QJSValue qmlAcceptedEvent(QJSEngine *engine, const ConfigSourceAccepted &event,
                          const ConfigSchema &schema);
QJSValue qmlProjectionEvent(QJSEngine *engine, const ConfigProjectionEffectsEvent &event);
QVariant qmlResultField(const ConfigResult &result, const ConfigDiagnosticBuilder &diagnostics,
                        const QString &field);
QStringList qmlResultKeys();
} // namespace configqml
