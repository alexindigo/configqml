// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlvalue.h"
#include "confignumber.h"
#include <QJSValue>
#include <QJSValueIterator>
#include <QRegularExpression>
#include <cmath>

namespace configqml {
static ConfigValueResult capture(const QVariant &input, const ConfigPath &path,
                                 QList<QJSValue> ancestors, int depth) {
    auto fail = [&](const char *code) {
        return ConfigValueResult{{}, {{path, QString::fromLatin1(code)}}};
    };
    if (depth > 64)
        return fail("value-depth");
    QVariant value = input;
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        const auto js = value.value<QJSValue>();
        if (js.isQObject() || js.isQMetaObject() || js.isCallable() || js.isDate() ||
            js.isRegExp() || js.isError())
            return fail("engine-bound-value");
        if (js.isObject()) {
            for (const auto &ancestor : ancestors)
                if (js.strictlyEquals(ancestor))
                    return fail("cyclic-value");
            ancestors.append(js);
        }
        value = js.toVariant(QJSValue::RetainJSObjects);
        if (value.metaType() == QMetaType::fromType<QJSValue>()) {
            if (js.isArray()) {
                QVariantList list;
                const auto length = js.property("length").toUInt();
                for (quint32 i = 0; i < length; ++i) {
                    auto child =
                        capture(QVariant::fromValue(js.property(i)),
                                path + ConfigPath{QString::number(i)}, ancestors, depth + 1);
                    if (!child.ok())
                        return child;
                    list.append(child.value);
                }
                return {list, {}};
            }
            if (!js.isObject())
                return fail("unsupported-value");
            QVariantMap map;
            QJSValueIterator it(js);
            while (it.hasNext()) {
                it.next();
                auto child = capture(QVariant::fromValue(it.value()), path + ConfigPath{it.name()},
                                     ancestors, depth + 1);
                if (!child.ok())
                    return child;
                map.insert(it.name(), child.value);
            }
            return {map, {}};
        }
    }
    if (value.metaType().id() == QMetaType::QVariantList ||
        value.metaType().id() == QMetaType::QStringList) {
        QVariantList list;
        const auto source = value.toList();
        for (qsizetype i = 0; i < source.size(); ++i) {
            auto child =
                capture(source[i], path + ConfigPath{QString::number(i)}, ancestors, depth + 1);
            if (!child.ok())
                return child;
            list.append(child.value);
        }
        return {list, {}};
    }
    if (value.metaType().id() == QMetaType::QVariantMap) {
        auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            auto child = capture(it.value(), path + ConfigPath{it.key()}, ancestors, depth + 1);
            if (!child.ok())
                return child;
            it.value() = child.value;
        }
        return {map, {}};
    }
    return ownValue(value, path);
}
ConfigValueResult captureQmlValue(const QVariant &value, const ConfigPath &path) {
    return capture(value, path, {}, 0);
}
static ConfigValueResult normalize(const ConfigDescriptor &d, const QVariant &v,
                                   const ConfigPath &path) {
    using T = ConfigDescriptor::Type;
    auto fail = [&](const char *code) {
        return ConfigValueResult{{}, {{path, QString::fromLatin1(code)}}};
    };
    if (isNull(v))
        return {v, {}};
    if ((d.type == T::List || d.type == T::Map) && d.element.size() != 1)
        return fail("element-descriptor");
    if (d.type == T::Integer || d.type == T::UnsignedInteger) {
        if (v.metaType().id() != QMetaType::QString)
            return fail("integer64-requires-decimal-string");
        const auto text = v.toString();
        static const QRegularExpression decimal("^-?(0|[1-9][0-9]*)$");
        if (!decimal.match(text).hasMatch() ||
            (d.type == T::UnsignedInteger && text.startsWith('-')))
            return fail("integer64-decimal-syntax");
        const auto token = ConfigNumber::fromJson(text);
        return token ? token->integer(d.type == T::UnsignedInteger, path)
                     : fail("integer64-decimal-syntax");
    }
    if (d.type == T::SafeInteger && v.metaType().id() == QMetaType::Double) {
        const double n = v.toDouble();
        if (std::trunc(n) != n)
            return fail("fractional-integer");
        if (n < -9007199254740991.0 || n > 9007199254740991.0)
            return fail("unsafe-integer");
        return {QVariant::fromValue(qint64(n)), {}};
    }
    if (d.type == T::Real &&
        (v.metaType().id() == QMetaType::LongLong || v.metaType().id() == QMetaType::Int))
        return {v.toDouble(), {}};
    if ((d.type == T::List && v.metaType().id() == QMetaType::QVariantList) ||
        ((d.type == T::Map || d.type == T::Record) &&
         v.metaType().id() == QMetaType::QVariantMap)) {
        if (d.type == T::List) {
            auto list = v.toList();
            for (qsizetype i = 0; i < list.size(); ++i) {
                auto child =
                    normalize(d.element.first(), list[i], path + ConfigPath{QString::number(i)});
                if (!child.ok())
                    return child;
                list[i] = child.value;
            }
            return {list, {}};
        }
        auto map = v.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (d.type == T::Record && !d.fields.contains(it.key()))
                continue;
            const auto childDescriptor =
                d.type == T::Map ? d.element.first() : d.fields.value(it.key());
            auto child = normalize(childDescriptor, it.value(), path + ConfigPath{it.key()});
            if (!child.ok())
                return child;
            it.value() = child.value;
        }
        return {map, {}};
    }
    return {v, {}};
}
ConfigValueResult admitQmlValue(const ConfigDescriptor &d, const QVariant &input,
                                const ConfigPath &path) {
    auto owned = captureQmlValue(input, path);
    if (!owned.ok())
        return owned;
    auto value = normalize(d, owned.value, path);
    return value.ok() ? d.validate(value.value, path) : value;
}
QVariant qmlValue(const ConfigDescriptor &d, const QVariant &v) {
    using T = ConfigDescriptor::Type;
    if (!v.isValid() || isNull(v))
        return v;
    if (d.type == T::Integer || d.type == T::UnsignedInteger)
        return v.toString();
    if (d.type == T::List) {
        auto list = v.toList();
        for (auto &item : list)
            item = qmlValue(d.element.first(), item);
        return list;
    }
    if (d.type == T::Record || d.type == T::Map) {
        auto map = v.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (d.type == T::Map)
                it.value() = qmlValue(d.element.first(), it.value());
            else if (d.fields.contains(it.key()))
                it.value() = qmlValue(d.fields.value(it.key()), it.value());
        return map;
    }
    return v;
}
QVariantMap qmlResult(const ConfigResult &r) {
    QVariantMap covered;
    for (auto it = r.coveredRevisions.cbegin(); it != r.coveredRevisions.cend(); ++it)
        covered[it.key()] = QString::fromLatin1(it->token);
    QVariantList publications;
    for (const auto &p : r.publications)
        publications.append(QVariantMap{{"sourceId", p.sourceId},
                                        {"success", p.success},
                                        {"revision", QString::fromLatin1(p.revision.token)},
                                        {"publicationState", p.state},
                                        {"error", p.error.code}});
    return {{"requestId", QString::number(r.requestId)},
            {"storeId", r.storeId},
            {"operation", r.operation},
            {"status", r.status},
            {"inputPrefix", QString::number(r.inputPrefix)},
            {"coveredRevisions", covered},
            {"publications", publications},
            {"publicationState", r.publicationState},
            {"error", QVariantMap{{"code", r.error.code},
                                  {"sourceId", r.error.sourceId},
                                  {"operation", r.error.operation}}},
            {"maskedBy", r.maskedBy},
            {"description", r.description()}};
}
} // namespace configqml
