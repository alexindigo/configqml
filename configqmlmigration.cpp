// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmldeclaration.h"
#include "confignumber.h"
#include <QJSEngine>
#include <QJSValueIterator>
#include <QUrl>
#include <cmath>

namespace configqml {
namespace {
// All engine values stay in this stack frame, on the worker. Raw atomic wrappers
// are plain frozen JS records, identified by identity, never key shape.
class MigrationEngine {
public:
    QJSEngine engine;
    QList<QPair<QJSValue, QVariant>> atoms;
    QList<QJSValue> ancestors;
    QString error;
    QJSValue dictionary() {
        // Null-prototype data dictionaries retain even __proto__ as an own key.
        return engine.globalObject().property("Object").property("create").call(
            {QJSValue(QJSValue::NullValue)});
    }
    void define(QJSValue object, const QString &key, const QJSValue &value) {
        auto descriptor = engine.newObject();
        descriptor.setProperty("value", value);
        descriptor.setProperty("enumerable", true);
        descriptor.setProperty("writable", true);
        descriptor.setProperty("configurable", true);
        engine.globalObject()
            .property("Object")
            .property("defineProperty")
            .call({object, key, descriptor});
    }
    QJSValue input(const QVariant &value) {
        const auto type = value.metaType().id();
        const bool token = value.metaType() == QMetaType::fromType<ConfigNumber>();
        const bool integer = type == QMetaType::Int || type == QMetaType::UInt ||
                             type == QMetaType::LongLong || type == QMetaType::ULongLong;
        const bool real = type == QMetaType::Float || type == QMetaType::Double;
        if (token || integer || real || type == QMetaType::QUrl) {
            auto object = dictionary();
            if (type == QMetaType::QUrl)
                define(object, "url", value.toUrl().toString(QUrl::FullyEncoded));
            else {
                const auto decimal = token ? value.value<ConfigNumber>().token()
                                     : real
                                         ? (value.toDouble() == 0 && std::signbit(value.toDouble())
                                                ? QString("-0")
                                                : QString::number(value.toDouble(), 'g', 17))
                                         : value.toString();
                define(object, "decimal", decimal);
            }
            engine.globalObject().property("Object").property("freeze").call({object});
            atoms.append({object, value});
            return object;
        }
        if (value.metaType().id() == QMetaType::QVariantMap) {
            auto object = dictionary();
            const auto map = value.toMap();
            for (auto it = map.cbegin(); it != map.cend(); ++it)
                define(object, it.key(), input(it.value()));
            return object;
        }
        if (value.metaType().id() == QMetaType::QVariantList) {
            const auto list = value.toList();
            auto array = engine.newArray(list.size());
            for (int i = 0; i < list.size(); ++i)
                array.setProperty(i, input(list[i]));
            return array;
        }
        return engine.toScriptValue(value);
    }
    QVariant output(const QJSValue &value, int depth = 0) {
        if (depth > 64 || value.isUndefined() || value.isCallable() || value.isQObject() ||
            value.isQMetaObject() || value.isDate() || value.isRegExp() || value.isError()) {
            error = "migration-value";
            return {};
        }
        if (value.isObject()) {
            for (const auto &atom : atoms)
                if (value.strictlyEquals(atom.first))
                    return atom.second;
            for (const auto &ancestor : ancestors)
                if (value.strictlyEquals(ancestor)) {
                    error = "migration-cycle";
                    return {};
                }
            ancestors.append(value);
            QVariant result;
            if (value.isArray()) {
                QVariantList list;
                const auto size = value.property("length").toUInt();
                for (quint32 i = 0; i < size; ++i)
                    list.append(output(value.property(i), depth + 1));
                result = list;
            } else {
                QVariantMap map;
                QJSValueIterator it(value);
                while (it.hasNext()) {
                    it.next();
                    map[it.name()] = output(it.value(), depth + 1);
                }
                result = map;
            }
            ancestors.removeLast();
            return result;
        }
        if (value.isNumber()) {
            const auto number = value.toNumber();
            if (!std::isfinite(number)) {
                error = "migration-number";
                return {};
            }
            const auto decimal = number == 0 && std::signbit(number)
                                     ? QString("-0")
                                     : QString::number(number, 'g', 17);
            return QVariant::fromValue(*ConfigNumber::fromJson(decimal));
        }
        return value.toVariant();
    }
};
} // namespace
QList<ConfigMigration> ConfigQmlSchema::compileMigrations(QList<ConfigDiagnostic> &errors) const {
    QList<ConfigMigration> result;
    if (migrations.isUndefined() || migrations.isNull())
        return result;
    if (!migrations.isArray()) {
        errors.append(ConfigDiagnosticBuilder().make({}, "migration-descriptor"));
        return result;
    }
    const auto size = migrations.property("length").toUInt();
    for (quint32 i = 0; i < size; ++i) {
        const auto migration = migrations.property(i);
        const auto from = migration.property("fromVersion");
        const auto function = migration.property("transform");
        if (!from.isNumber() || from.toNumber() != from.toInt() || !function.isString()) {
            errors.append(ConfigDiagnosticBuilder().make({}, "migration-descriptor"));
            continue;
        }
        const auto source = ownString(function.toString()); // Explicit standalone JS source.
        result.append({from.toInt(), [source](const QVariantMap &raw) {
                           MigrationEngine worker;
                           const auto function = worker.engine.evaluate("(" + source + ")");
                           if (!function.isCallable())
                               return ConfigValueResult{
                                   {}, {ConfigDiagnosticBuilder().make({}, "migration-source")}};
                           const auto output = function.call({worker.input(raw)});
                           if (output.isError() || worker.engine.hasError())
                               return ConfigValueResult{
                                   {}, {ConfigDiagnosticBuilder().make({}, "migration-script")}};
                           const auto value = worker.output(output);
                           if (!worker.error.isEmpty())
                               return ConfigValueResult{
                                   {}, {ConfigDiagnosticBuilder().make({}, worker.error)}};
                           return ConfigValueResult{value, {}};
                       }});
    }
    return result;
}
} // namespace configqml
