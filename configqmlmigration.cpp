// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmldeclaration.h"
#include "confignumber.h"
#include "configqmlvalue.h"
#include "configsourceprojection_p.h"
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
    QString protocol = "migration";
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
            error = protocol == "projection"
                        ? (depth > 64 ? "projection-depth" : "projection-result")
                        : "migration-value";
            return {};
        }
        if (value.isObject()) {
            for (const auto &atom : atoms)
                if (value.strictlyEquals(atom.first))
                    return atom.second;
            for (const auto &ancestor : ancestors)
                if (value.strictlyEquals(ancestor)) {
                    error = protocol + "-cycle";
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
                error = protocol + "-number";
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
ConfigProjectionSpec ConfigQmlProjection::compile(QList<ConfigDiagnostic> &errors) const {
    ConfigProjectionSpec spec;
    spec.projectionId = ownString(projectionId);
    for (const auto &id : sourceIds)
        spec.sourceIds.append(ownString(id));
    if (!transform.isString() || transform.toString().isEmpty()) {
        errors.append(ConfigDiagnosticBuilder().make({}, "projection-declaration"));
        return spec;
    }
    if (!metadata.isUndefined()) {
        const auto captured = captureQmlValue(QVariant::fromValue(metadata));
        if (!captured.ok() || captured.value.metaType().id() != QMetaType::QVariantMap) {
            errors.append(ConfigDiagnosticBuilder().make({}, "projection-declaration"));
            return spec;
        }
        spec.metadata = captured.value.toMap();
    }
    const auto source = ownString(transform.toString());
    spec.transform = [source](const ConfigProjectionInput &input) {
        MigrationEngine worker;
        worker.protocol = "projection";
        const auto function = worker.engine.evaluate("(" + source + ")");
        if (!function.isCallable() || worker.engine.hasError())
            throw ConfigProjectionFailure("projection-source");
        auto record = worker.dictionary();
        for (const auto &member :
             {QString("raw"), QString("defaults"), QString("lowerValues"), QString("values"),
              QString("compatibilityRaw"), QString("metadata")}) {
            const auto value = member == "raw"                ? input.raw
                               : member == "defaults"         ? input.defaults
                               : member == "lowerValues"      ? input.lowerValues
                               : member == "values"           ? input.values
                               : member == "compatibilityRaw" ? input.compatibilityRaw
                                                              : input.metadata;
            worker.define(record, member, worker.input(value));
        }
        worker.define(record, "source",
                      worker.input(QVariantMap{{"sourceId", input.source.sourceId},
                                               {"layerId", input.source.layerId},
                                               {"prefix", input.source.prefix},
                                               {"presence", input.source.presence},
                                               {"revision", input.source.revision}}));
        worker.define(record, "schemaVersion", input.schemaVersion);
        auto prior = worker.dictionary();
        worker.define(prior, "present", bool(input.priorVersion));
        if (input.priorVersion)
            worker.define(prior, "value", QString::number(*input.priorVersion));
        worker.define(record, "priorVersion", prior);
        const auto output = function.call({record});
        if (output.isError() || worker.engine.hasError())
            throw ConfigProjectionFailure("projection-script");
        auto exact = [](const QJSValue &object, const QStringList &keys) {
            if (!object.isObject() || object.isArray() || object.isCallable() || object.isQObject())
                return false;
            QStringList found;
            QJSValueIterator it(object);
            while (it.hasNext()) {
                it.next();
                found.append(it.name());
            }
            auto expected = keys;
            found.sort();
            expected.sort();
            return found == expected;
        };
        auto dictionary = [](const QJSValue &value) {
            return value.isObject() && !value.isArray() && !value.isCallable() &&
                   !value.isQObject() && !value.isDate() && !value.isRegExp();
        };
        if (!exact(output, {"values", "compatibilityRaw", "effects", "diagnostics"}) ||
            !dictionary(output.property("values")) ||
            !dictionary(output.property("compatibilityRaw")) ||
            !output.property("effects").isArray() || !output.property("diagnostics").isArray())
            throw ConfigProjectionFailure("projection-result");
        ConfigProjectionResult result;
        const auto values = worker.output(output.property("values"));
        const auto compatibilityRaw = worker.output(output.property("compatibilityRaw"));
        if (!worker.error.isEmpty())
            throw ConfigProjectionFailure(worker.error);
        if (values.metaType().id() != QMetaType::QVariantMap ||
            compatibilityRaw.metaType().id() != QMetaType::QVariantMap)
            throw ConfigProjectionFailure("projection-result");
        result.values = values.toMap();
        result.compatibilityRaw = compatibilityRaw.toMap();
        const auto effects = output.property("effects");
        const auto diagnostics = output.property("diagnostics");
        if (effects.property("length").toUInt() > 64)
            throw ConfigProjectionFailure("projection-effects");
        if (diagnostics.property("length").toUInt() > 64)
            throw ConfigProjectionFailure("projection-diagnostics");
        for (quint32 i = 0; i < effects.property("length").toUInt(); ++i) {
            const auto effect = effects.property(i);
            const auto version = effect.property("version");
            const auto n = version.toNumber();
            if (!exact(effect, {"effectId", "version", "action", "arguments"}) ||
                !effect.property("effectId").isString() || !effect.property("action").isString() ||
                !dictionary(effect.property("arguments")) || !version.isNumber() ||
                !std::isfinite(n) || std::trunc(n) != n || n < 0 || n > 9007199254740991.0)
                throw ConfigProjectionFailure("projection-effects");
            const auto arguments = worker.output(effect.property("arguments"));
            if (!worker.error.isEmpty())
                throw ConfigProjectionFailure(worker.error);
            if (arguments.metaType().id() != QMetaType::QVariantMap)
                throw ConfigProjectionFailure("projection-effects");
            result.effects.append({effect.property("effectId").toString(), quint64(n),
                                   effect.property("action").toString(), arguments.toMap()});
        }
        for (quint32 i = 0; i < diagnostics.property("length").toUInt(); ++i) {
            const auto diagnostic = diagnostics.property(i);
            if (!exact(diagnostic, {"code", "path", "severity"}) ||
                !diagnostic.property("code").isString() ||
                !diagnostic.property("severity").isString() ||
                !diagnostic.property("path").isArray())
                throw ConfigProjectionFailure("projection-diagnostics");
            ConfigPath path;
            const auto segments = diagnostic.property("path");
            for (quint32 j = 0; j < segments.property("length").toUInt(); ++j) {
                if (!segments.property(j).isString())
                    throw ConfigProjectionFailure("projection-diagnostics");
                path.append(segments.property(j).toString());
            }
            result.diagnostics.append({diagnostic.property("code").toString(), path,
                                       diagnostic.property("severity").toString()});
        }
        if (worker.engine.hasError())
            throw ConfigProjectionFailure("projection-script");
        return result;
    };
    return spec;
}
} // namespace configqml
