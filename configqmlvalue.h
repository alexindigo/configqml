// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include "configresult.h"
namespace configqml {
ConfigValueResult captureQmlValue(const QVariant &value, const ConfigPath &path = {});
ConfigAdmission admitQmlInput(const ConfigSchema &schema, const ConfigPath &path,
                              const QVariant &value);
ConfigValueResult admitQmlValue(const ConfigDescriptor &descriptor, const QVariant &value,
                                const ConfigPath &path = {});
QVariant qmlValue(const ConfigDescriptor &descriptor, const QVariant &value);
QVariantMap qmlResult(const ConfigResult &result);
} // namespace configqml
