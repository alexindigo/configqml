// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configvalue.h"
#include "confignumber.h"
#include <QUrl>
#include <cmath>

namespace configqml {
const char *version() { return "0.1.0"; }

bool isNull(const QVariant &value) { return value.metaType().id() == QMetaType::Nullptr; }

ConfigPath ownPath(const ConfigPath &path) {
    ConfigPath out;
    for (const auto &key : path)
        out.append(ownString(key));
    return out;
}
QString ownString(const QString &value) { return QString(value.constData(), value.size()); }

static ConfigValueResult capture(const QVariant &in, const ConfigPath &path, int depth,
                                 const ConfigDiagnosticBuilder &diagnostics) {
    auto fail = [&](const char *code) {
        return ConfigValueResult{{}, {diagnostics.make(path, QString::fromLatin1(code))}};
    };
    if (depth > 64)
        return fail("value-depth");
    if (in.metaType() == QMetaType::fromType<ConfigNumber>()) {
        const auto number = ConfigNumber::fromJson(in.value<ConfigNumber>().token());
        return number ? ConfigValueResult{QVariant::fromValue(*number), {}} : fail("number-syntax");
    }
    switch (in.metaType().id()) {
    case QMetaType::Nullptr:
        return {QVariant::fromValue(nullptr), {}};
    case QMetaType::Bool:
        return {in.toBool(), {}};
    case QMetaType::Int:
    case QMetaType::LongLong:
        return {QVariant::fromValue(in.toLongLong()), {}};
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        return {QVariant::fromValue(in.toULongLong()), {}};
    case QMetaType::Float:
    case QMetaType::Double:
        if (!std::isfinite(in.toDouble()))
            return fail("non-finite");
        return {in.toDouble(), {}};
    case QMetaType::QString: {
        const auto text = in.toString();
        return {QString(text.constData(), text.size()), {}};
    }
    case QMetaType::QUrl: {
        const auto url = in.toUrl();
        if (!url.isValid())
            return fail("invalid-url");
        return {QUrl::fromEncoded(url.toEncoded(QUrl::FullyEncoded), QUrl::StrictMode), {}};
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        QVariantList out;
        ConfigValueResult result;
        const auto list = in.toList();
        for (qsizetype i = 0; i < list.size(); ++i) {
            auto item =
                capture(list[i], path + ConfigPath{QString::number(i)}, depth + 1, diagnostics);
            result.errors.append(item.errors);
            out.append(item.value);
        }
        if (result.ok())
            result.value = out;
        return result;
    }
    case QMetaType::QVariantMap: {
        QVariantMap out;
        ConfigValueResult result;
        const auto map = in.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            auto item = capture(it.value(), path + ConfigPath{it.key()}, depth + 1, diagnostics);
            result.errors.append(item.errors);
            out.insert(QString(it.key().constData(), it.key().size()), item.value);
        }
        if (result.ok())
            result.value = out;
        return result;
    }
    default:
        return fail("unsupported-value");
    }
}

ConfigValueResult ownValue(const QVariant &input, const ConfigPath &path,
                           const ConfigDiagnosticBuilder &diagnostics) {
    return capture(input, path, 0, diagnostics);
}

bool startsWith(const ConfigPath &path, const ConfigPath &prefix) {
    return path.size() >= prefix.size() && path.mid(0, prefix.size()) == prefix;
}

std::optional<QVariant> valueAt(const QVariantMap &tree, const ConfigPath &path) {
    QVariant value = tree;
    for (const auto &key : path) {
        if (value.metaType().id() != QMetaType::QVariantMap)
            return std::nullopt;
        const auto map = value.toMap();
        auto it = map.constFind(key);
        if (it == map.cend())
            return std::nullopt;
        value = *it;
    }
    return value;
}

void putValue(QVariantMap &tree, const ConfigPath &path, const QVariant &value) {
    Q_ASSERT(!path.isEmpty());
    if (path.size() == 1) {
        tree.insert(ownString(path.first()), value);
        return;
    }
    auto child = tree.value(path.first()).toMap();
    putValue(child, path.mid(1), value);
    tree.insert(ownString(path.first()), child);
}

void removeValue(QVariantMap &tree, const ConfigPath &path) {
    Q_ASSERT(!path.isEmpty());
    if (path.size() == 1) {
        tree.remove(path.first());
        return;
    }
    if (tree.value(path.first()).metaType().id() != QMetaType::QVariantMap)
        return;
    auto child = tree.value(path.first()).toMap();
    removeValue(child, path.mid(1));
    // Empty namespaces are retained in raw data; they are not delete tokens.
    tree.insert(ownString(path.first()), child);
}
} // namespace configqml
