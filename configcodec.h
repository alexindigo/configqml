// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configbackend.h"
#include "configvalue.h"
#include "confignumber.h"

namespace configqml {
struct ConfigCodecCapabilities {
    bool decode = true;
    bool encode = false;
    bool preservesUnknownKeys = false;
    bool preservesFormatting = false;
    bool losslessSigned64 = false;
    bool losslessUnsigned64 = false;
    bool urls = false;
    bool nullable = false;
    ConfigBackendCapabilities::Affinity affinity = ConfigBackendCapabilities::Affinity::SingleOwner;
};
struct ConfigDecodeResult {
    QVariantMap raw;
    ConfigError error;
    bool ok() const { return error.empty(); }
};
struct ConfigEncodeResult {
    QByteArray bytes;
    ConfigError error;
    bool ok() const { return error.empty(); }
};
// Codec is distinct from storage. A writable preserving codec encodes edited raw
// layers, never the schema/effective projection. Numeric decoding must retain
// ConfigNumber lexemes (decodeJsonValue/encodeJsonValue), or provide exact native
// integer/finite-real values without a lossy intermediate. Already-rounded JSON
// numbers cannot be repaired by schema validation. M3 supplies file storage.
class ConfigCodec {
public:
    virtual ~ConfigCodec() = default;
    virtual ConfigCodecCapabilities capabilities() const = 0;
    virtual ConfigDecodeResult decode(const QByteArray &bytes, const QString &sourceId) = 0;
    virtual ConfigEncodeResult encode(const QVariantMap &raw, const QString &sourceId) = 0;
};
} // namespace configqml
