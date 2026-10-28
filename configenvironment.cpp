// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configenvironment.h"
#include "confignumber.h"
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <cmath>

namespace configqml {
using T = ConfigDescriptor::Type;
// URL strings are an explicit env adapter convention. Numeric admission belongs
// to the shared schema boundary, with original decimal tokens still intact.
static QVariant jsonValue(const QVariant &v, const ConfigDescriptor &d) {
    if (v.metaType().id() == QMetaType::QVariantList && d.type == T::List &&
        d.element.size() == 1) {
        QVariantList out;
        for (const auto &item : v.toList())
            out.append(jsonValue(item, d.element.first()));
        return out;
    }
    if (v.metaType().id() == QMetaType::QVariantMap && (d.type == T::Record || d.type == T::Map)) {
        QVariantMap out;
        const auto map = v.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (d.type == T::Map && d.element.size() == 1)
                out.insert(it.key(), jsonValue(it.value(), d.element.first()));
            else if (d.fields.contains(it.key()))
                out.insert(it.key(), jsonValue(it.value(), d.fields.value(it.key())));
            else
                out.insert(it.key(), it.value());
        }
        return out;
    }
    if (v.metaType().id() == QMetaType::QString && d.type == T::Url)
        return QUrl(v.toString(), QUrl::StrictMode);
    return v;
}

static ConfigValueResult parse(const QString &text, const ConfigDescriptor &d,
                               const ConfigPath &path) {
    static const QRegularExpression integer("\\A-?(?:0|[1-9][0-9]*)\\z");
    static const QRegularExpression unsignedInteger("\\A(?:0|[1-9][0-9]*)\\z");
    static const QRegularExpression real(
        "\\A-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\\z");
    auto fail = [&]() { return ConfigValueResult{{}, {{path, "environment-syntax"}}}; };
    QVariant value;
    switch (d.type) {
    case T::String:
        value = text;
        break;
    case T::Boolean:
        if (text != "true" && text != "false")
            return fail();
        value = text == "true";
        break;
    case T::Integer:
        if (!integer.match(text).hasMatch())
            return fail();
        value = QVariant::fromValue(*ConfigNumber::fromJson(text));
        break;
    case T::UnsignedInteger:
        if (!unsignedInteger.match(text).hasMatch())
            return fail();
        value = QVariant::fromValue(*ConfigNumber::fromJson(text));
        break;
    case T::Real:
        if (!real.match(text).hasMatch())
            return fail();
        value = QVariant::fromValue(*ConfigNumber::fromJson(text));
        break;
    case T::Url:
        value = QUrl(text, QUrl::StrictMode);
        break;
    case T::List:
    case T::Record:
    case T::Map: {
        const auto parsed = decodeJsonValue(text.toUtf8(), path);
        if (!parsed.ok())
            return parsed;
        value = jsonValue(parsed.value, d);
        break;
    }
    case T::Group:
        return fail();
    }
    return d.validate(value, path);
}

ConfigEnvironmentResult mapEnvironment(const ConfigSchema &schema,
                                       const QList<ConfigEnvironmentMapping> &mappings,
                                       const QMap<QString, QString> &environment) {
    ConfigEnvironmentResult out;
    if (!schema.valid()) {
        out.errors = schema.errors();
        return out;
    }
    QSet<QString> names;
    QSet<ConfigPath> paths;
    // Validate the mapping table independently of which variables happen to be present.
    for (const auto &mapping : mappings) {
        const auto d = schema.descriptor(mapping.path);
        if (mapping.name.isEmpty() || names.contains(mapping.name) ||
            paths.contains(mapping.path) || !d || d->type == T::Group || d->forbidden)
            out.errors.append(ConfigDiagnostic{mapping.path, "environment-mapping"});
        names.insert(mapping.name);
        paths.insert(mapping.path);
    }
    if (!out.ok())
        return out;
    for (const auto &mapping : mappings) {
        if (!environment.contains(mapping.name))
            continue;
        const auto result = parse(environment.value(mapping.name), *schema.descriptor(mapping.path),
                                  ownPath(mapping.path));
        out.errors.append(result.errors);
        if (result.ok())
            putValue(out.values, ownPath(mapping.path), result.value);
    }
    return out;
}

std::optional<QList<ConfigEnvironmentMapping>> environmentMappings(const ConfigSchema &schema,
                                                                   const QString &prefix,
                                                                   const QList<ConfigPath> &paths) {
    static const QRegularExpression simple("\\A[a-z][a-z0-9]*\\z");
    static const QRegularExpression prefixGrammar("\\A[A-Z][A-Z0-9]*\\z");
    if (!prefixGrammar.match(prefix).hasMatch())
        return std::nullopt;
    QList<ConfigEnvironmentMapping> mappings;
    for (const auto &path : paths) {
        if (path.isEmpty())
            return std::nullopt;
        for (const auto &key : path)
            if (!simple.match(key).hasMatch())
                return std::nullopt;
        mappings.append({prefix + "_" + path.join('_').toUpper(), ownPath(path)});
    }
    return mapEnvironment(schema, mappings, {}).ok() ? std::optional(mappings) : std::nullopt;
}
} // namespace configqml
