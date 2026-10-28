// SPDX-License-Identifier: LGPL-3.0-or-later
#include "confignumber.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace configqml {
std::optional<ConfigNumber> ConfigNumber::fromJson(const QString &token) {
    static const QRegularExpression grammar(
        "\\A-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\\z");
    if (!grammar.match(token).hasMatch())
        return std::nullopt;
    ConfigNumber number;
    number.m_token = QString(token.constData(), token.size());
    return number;
}

ConfigValueResult ConfigNumber::integer(bool unsignedType, const ConfigPath &path) const {
    auto fail = [&](const char *code) {
        return ConfigValueResult{{}, {{path, QString::fromLatin1(code)}}};
    };
    if (!fromJson(m_token))
        return fail("number-syntax");
    const bool negative = m_token.startsWith('-');
    QString coefficient = negative ? m_token.mid(1) : m_token;
    const auto exponentAt = coefficient.indexOf('e', 0, Qt::CaseInsensitive);
    qint64 exponent = 0;
    if (exponentAt >= 0) {
        auto text = coefficient.mid(exponentAt + 1);
        coefficient.truncate(exponentAt);
        const bool minus = text.startsWith('-');
        if (minus || text.startsWith('+'))
            text.remove(0, 1);
        // Saturation avoids arithmetic overflow for arbitrarily long exponents.
        // Beyond this input-relative bound a nonzero value cannot fit an integer.
        const qint64 bound = m_token.size() + 21;
        for (const auto digit : text) {
            exponent = qMin(bound, exponent * 10 + digit.digitValue());
            if (exponent == bound)
                break;
        }
        if (minus)
            exponent = -exponent;
    }
    const auto point = coefficient.indexOf('.');
    const qint64 fractionalDigits = point < 0 ? 0 : coefficient.size() - point - 1;
    coefficient.remove('.');
    while (coefficient.startsWith('0'))
        coefficient.remove(0, 1);
    if (coefficient.isEmpty())
        return {unsignedType ? QVariant::fromValue(quint64(0)) : QVariant::fromValue(qint64(0)),
                {}};
    const qint64 scale = exponent - fractionalDigits;
    if (scale < 0) {
        if (-scale >= coefficient.size())
            return fail("fractional-integer");
        for (qint64 i = coefficient.size() + scale; i < coefficient.size(); ++i)
            if (coefficient[i] != '0')
                return fail("fractional-integer");
        coefficient.chop(-scale);
    } else {
        if (coefficient.size() + scale > 20)
            return fail("integer-range");
        coefficient.append(QString(scale, '0'));
    }
    if (unsignedType && negative)
        return fail("integer-range");
    const QString limit = unsignedType ? QStringLiteral("18446744073709551615")
                          : negative   ? QStringLiteral("9223372036854775808")
                                       : QStringLiteral("9223372036854775807");
    if (coefficient.size() > limit.size() ||
        (coefficient.size() == limit.size() && coefficient > limit))
        return fail("integer-range");
    bool ok = false;
    const QVariant value =
        unsignedType
            ? QVariant::fromValue(coefficient.toULongLong(&ok))
            : QVariant::fromValue((negative ? "-" + coefficient : coefficient).toLongLong(&ok));
    return ok ? ConfigValueResult{value, {}} : fail("integer-range");
}

ConfigValueResult ConfigNumber::real(const ConfigPath &path) const {
    bool ok = false;
    const double value = m_token.toDouble(&ok);
    if (!fromJson(m_token) || !ok || !std::isfinite(value))
        return {{}, {{path, "real-range"}}};
    return {value, {}};
}

namespace {
class JsonReader {
public:
    explicit JsonReader(const QByteArray &bytes, ConfigJsonLayout *layout)
        : bytes(bytes), layout(layout) {}
    ConfigValueResult run(const ConfigPath &path) {
        auto value = read(path, 0);
        space();
        if (offset != bytes.size() && errors.isEmpty())
            fail(path);
        return {errors.isEmpty() ? value : QVariant(), errors};
    }

private:
    const QByteArray &bytes;
    ConfigJsonLayout *layout;
    qsizetype offset = 0;
    QList<ConfigDiagnostic> errors;
    void space() {
        while (offset < bytes.size() && (bytes[offset] == ' ' || bytes[offset] == '\t' ||
                                         bytes[offset] == '\r' || bytes[offset] == '\n'))
            ++offset;
    }
    bool consume(char ch) {
        space();
        if (offset < bytes.size() && bytes[offset] == ch) {
            ++offset;
            return true;
        }
        return false;
    }
    QVariant fail(const ConfigPath &path, const char *code = "json-syntax") {
        if (errors.isEmpty()) {
            errors.append({path, QString::fromLatin1(code)});
            if (layout)
                layout->errorOffset = offset;
        }
        return {};
    }
    std::optional<QString> string(const ConfigPath &path) {
        space();
        const auto start = offset;
        if (offset >= bytes.size() || bytes[offset++] != '"') {
            fail(path);
            return std::nullopt;
        }
        bool closed = false;
        while (offset < bytes.size()) {
            const char ch = bytes[offset++];
            if (ch == '\\') {
                if (offset >= bytes.size()) {
                    fail(path);
                    return std::nullopt;
                }
                const char escaped = bytes[offset++];
                if (escaped == 'u') {
                    for (int i = 0; i < 4; ++i) {
                        if (offset >= bytes.size() ||
                            !((bytes[offset] >= '0' && bytes[offset] <= '9') ||
                              (bytes[offset] >= 'a' && bytes[offset] <= 'f') ||
                              (bytes[offset] >= 'A' && bytes[offset] <= 'F'))) {
                            fail(path);
                            return std::nullopt;
                        }
                        ++offset;
                    }
                } else if (!QByteArray("\"\\/bfnrt").contains(escaped)) {
                    fail(path);
                    return std::nullopt;
                }
            } else if (ch == '"') {
                closed = true;
                break;
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                fail(path);
                return std::nullopt;
            }
        }
        if (!closed) {
            fail(path);
            return std::nullopt;
        }
        // Qt handles UTF-8 and string escapes only, never a numeric token.
        QJsonParseError error;
        const auto doc =
            QJsonDocument::fromJson("[" + bytes.mid(start, offset - start) + "]", &error);
        if (error.error != QJsonParseError::NoError || doc.array().size() != 1 ||
            !doc.array()[0].isString()) {
            fail(path);
            return std::nullopt;
        }
        return doc.array()[0].toString();
    }
    QVariant read(const ConfigPath &path, int depth) {
        space();
        if (depth > 64)
            return fail(path, "value-depth");
        if (offset >= bytes.size())
            return fail(path);
        if (bytes[offset] == '"') {
            auto text = string(path);
            return text ? QVariant(*text) : QVariant();
        }
        if (consume('[')) {
            QVariantList out;
            if (consume(']'))
                return out;
            do {
                out.append(read(path + ConfigPath{QString::number(out.size())}, depth + 1));
                if (!errors.isEmpty())
                    return {};
                if (consume(']'))
                    return out;
            } while (consume(','));
            return fail(path);
        }
        if (consume('{')) {
            QVariantMap out;
            if (consume('}'))
                return out;
            do {
                const auto key = string(path);
                if (!key || !consume(':'))
                    return fail(path);
                const auto child = path + ConfigPath{*key};
                if (out.contains(*key))
                    return fail(child, "json-duplicate-key");
                if (layout)
                    layout->keys[path].append(*key);
                out.insert(*key, read(child, depth + 1));
                if (!errors.isEmpty())
                    return {};
                if (consume('}'))
                    return out;
            } while (consume(','));
            return fail(path);
        }
        const auto start = offset;
        while (offset < bytes.size() && bytes[offset] != ',' && bytes[offset] != ']' &&
               bytes[offset] != '}' && bytes[offset] != ' ' && bytes[offset] != '\t' &&
               bytes[offset] != '\n' && bytes[offset] != '\r')
            ++offset;
        const auto atom = bytes.mid(start, offset - start);
        if (atom == "null")
            return QVariant::fromValue(nullptr);
        if (atom == "true" || atom == "false")
            return atom == "true";
        const auto number = ConfigNumber::fromJson(QString::fromLatin1(atom));
        return number ? QVariant::fromValue(*number) : fail(path);
    }
};

QByteArray quote(const QString &text) {
    const auto encoded = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return encoded.mid(1, encoded.size() - 2);
}
ConfigJsonEncodeResult encode(const QVariant &value, const ConfigPath &path) {
    if (value.metaType() == QMetaType::fromType<ConfigNumber>())
        return {value.value<ConfigNumber>().token().toLatin1(), {}};
    switch (value.metaType().id()) {
    case QMetaType::Nullptr:
        return {"null", {}};
    case QMetaType::Bool:
        return {value.toBool() ? "true" : "false", {}};
    case QMetaType::LongLong:
        return {QByteArray::number(value.toLongLong()), {}};
    case QMetaType::ULongLong:
        return {QByteArray::number(value.toULongLong()), {}};
    case QMetaType::Double: {
        const double real = value.toDouble();
        return {real == 0 && std::signbit(real)
                    ? QByteArray("-0.0")
                    : QByteArray::number(real, 'g', std::numeric_limits<double>::max_digits10),
                {}};
    }
    case QMetaType::QString:
        return {quote(value.toString()), {}};
    case QMetaType::QVariantList: {
        QByteArray bytes = "[";
        const auto list = value.toList();
        for (qsizetype i = 0; i < list.size(); ++i) {
            const auto item = encode(list[i], path + ConfigPath{QString::number(i)});
            if (!item.ok())
                return item;
            if (i)
                bytes += ',';
            bytes += item.bytes;
        }
        return {bytes + ']', {}};
    }
    case QMetaType::QVariantMap: {
        QByteArray bytes = "{";
        const auto map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            const auto item = encode(it.value(), path + ConfigPath{it.key()});
            if (!item.ok())
                return item;
            if (it != map.cbegin())
                bytes += ',';
            bytes += quote(it.key()) + ':' + item.bytes;
        }
        return {bytes + '}', {}};
    }
    default:
        return {{}, {{path, "json-unsupported-value"}}};
    }
}
} // namespace

ConfigValueResult decodeJsonValue(const QByteArray &bytes, const ConfigPath &path,
                                  ConfigJsonLayout *layout) {
    return JsonReader(bytes, layout).run(ownPath(path));
}
ConfigJsonEncodeResult encodeJsonValue(const QVariant &value, const ConfigPath &path) {
    const auto owned = ownValue(value, path);
    return owned.ok() ? encode(owned.value, path) : ConfigJsonEncodeResult{{}, owned.errors};
}
} // namespace configqml
