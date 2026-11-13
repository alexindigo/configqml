// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configqmlvalue.h"
#include <functional>

namespace configqml {
// The loader owns only immutable native event data, never the emitting store.
QJSValue qmlEvent(QJSEngine *engine, const QStringList &keys,
                  std::function<QVariant(const QString &)> loader);
QJSValue qmlResultEvent(QJSEngine *engine, const ConfigResult &result,
                        const ConfigDiagnosticBuilder &diagnostics = {});
QJSValue qmlErrorEvent(QJSEngine *engine, const ConfigError &error);
QJSValue qmlAcceptedEvent(QJSEngine *engine, const ConfigSourceAccepted &event,
                          const ConfigSchema &schema);
QJSValue qmlProjectionEvent(QJSEngine *engine, const ConfigProjectionEffectsEvent &event);
QVariant qmlResultField(const ConfigResult &result, const ConfigDiagnosticBuilder &diagnostics,
                        const QString &field);
QStringList qmlResultKeys();
} // namespace configqml
