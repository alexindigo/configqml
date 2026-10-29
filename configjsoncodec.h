// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configcodec.h"
#include "configschema.h"

namespace configqml {
QVariant jsonSettingValue(const QVariant &raw, const ConfigDescriptor &descriptor);
class ConfigJsonCodec : public ConfigCodec {
public:
    explicit ConfigJsonCodec(ConfigDiagnosticBuilder diagnostics = {})
        : m_diagnostics(std::move(diagnostics)) {}
    ConfigCodecCapabilities capabilities() const override;
    ConfigDecodeResult decode(const QByteArray &bytes, const QString &sourceId) override;
    ConfigEncodeResult encode(const QVariantMap &raw, const QString &sourceId) override;

private:
    ConfigJsonLayout m_layout;
    ConfigDiagnosticBuilder m_diagnostics;
};
} // namespace configqml
