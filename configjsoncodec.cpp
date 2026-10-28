// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsoncodec.h"
#include <QUrl>

namespace configqml {
QVariant jsonSettingValue(const QVariant &raw, const ConfigDescriptor &d) {
    using T = ConfigDescriptor::Type;
    if (raw.metaType().id() == QMetaType::QString && d.type == T::Url)
        return QUrl(raw.toString(), QUrl::StrictMode);
    if (raw.metaType().id() == QMetaType::QVariantList && d.type == T::List &&
        d.element.size() == 1) {
        auto list = raw.toList();
        for (auto &value : list)
            value = jsonSettingValue(value, d.element.first());
        return list;
    }
    if (raw.metaType().id() == QMetaType::QVariantMap &&
        (d.type == T::Record || d.type == T::Map)) {
        auto map = raw.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (d.type == T::Map && d.element.size() == 1)
                it.value() = jsonSettingValue(it.value(), d.element.first());
            else if (d.fields.contains(it.key()))
                it.value() = jsonSettingValue(it.value(), d.fields.value(it.key()));
        return map;
    }
    return raw;
}
ConfigCodecCapabilities ConfigJsonCodec::capabilities() const {
    ConfigCodecCapabilities c;
    c.encode = c.preservesUnknownKeys = c.losslessSigned64 = c.losslessUnsigned64 = true;
    c.urls = c.nullable = true;
    return c;
}
ConfigDecodeResult ConfigJsonCodec::decode(const QByteArray &bytes, const QString &id) {
    ConfigJsonLayout layout;
    const auto result = decodeJsonValue(bytes, {}, &layout);
    if (!result.ok()) {
        const auto d = result.errors.first();
        return {{},
                {d.code, id, "parse", {}, d.path, 1 + bytes.left(layout.errorOffset).count('\n')}};
    }
    if (result.value.metaType().id() != QMetaType::QVariantMap)
        return {{}, {"json-root-object", id, "parse", {}, {}, 1}};
    m_layout = layout;
    return {result.value.toMap(), {}};
}
static ConfigJsonEncodeResult ordered(const QVariant &value, const ConfigPath &path,
                                      const ConfigJsonLayout &layout) {
    if (value.metaType().id() == QMetaType::QUrl)
        return encodeJsonValue(value.toUrl().toString(QUrl::FullyEncoded), path);
    if (value.metaType().id() == QMetaType::QVariantMap) {
        const auto map = value.toMap();
        auto keys = layout.keys.value(path);
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            if (!keys.contains(it.key()))
                keys.append(it.key());
        QByteArray bytes = "{";
        bool first = true;
        for (const auto &key : keys) {
            if (!map.contains(key))
                continue;
            const auto encoded = ordered(map.value(key), path + ConfigPath{key}, layout);
            if (!encoded.ok())
                return encoded;
            if (!first)
                bytes += ',';
            first = false;
            bytes += encodeJsonValue(key).bytes + ':' + encoded.bytes;
        }
        return {bytes + '}', {}};
    }
    if (value.metaType().id() == QMetaType::QVariantList) {
        QByteArray bytes = "[";
        const auto list = value.toList();
        for (qsizetype i = 0; i < list.size(); ++i) {
            const auto encoded = ordered(list[i], path + ConfigPath{QString::number(i)}, layout);
            if (!encoded.ok())
                return encoded;
            if (i)
                bytes += ',';
            bytes += encoded.bytes;
        }
        return {bytes + ']', {}};
    }
    return encodeJsonValue(value, path);
}
ConfigEncodeResult ConfigJsonCodec::encode(const QVariantMap &raw, const QString &id) {
    const auto owned = ownValue(raw);
    if (!owned.ok())
        return {{}, {owned.errors.first().code, id, "encode", {}, owned.errors.first().path}};
    const auto result = ordered(owned.value, {}, m_layout);
    if (!result.ok())
        return {{}, {result.errors.first().code, id, "encode", {}, result.errors.first().path}};
    return {result.bytes + '\n', {}};
}
} // namespace configqml
