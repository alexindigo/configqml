// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmldeclaration.h"
#include "configqmlvalue.h"
namespace configqml {
ConfigDescriptor ConfigQmlGroup::compile(QList<ConfigDiagnostic> &errors,
                                         const ConfigPath &path) const {
    QMap<QString, ConfigDescriptor> fields;
    for (const auto *child : m_children) {
        if (child->key.isEmpty() || fields.contains(child->key))
            errors.append({path, "duplicate-or-empty-key"});
        fields[child->key] = child->compile(errors, path + ConfigPath{child->key});
    }
    return ConfigDescriptor::group(fields);
}
ConfigDescriptor ConfigQmlValue::compile(QList<ConfigDiagnostic> &errors,
                                         const ConfigPath &path) const {
    using T = ConfigDescriptor::Type;
    static const T types[] = {T::Boolean, T::SafeInteger, T::Integer, T::UnsignedInteger,
                              T::Real,    T::String,      T::Url,     T::List,
                              T::List,    T::Record,      T::Map};
    ConfigDescriptor d;
    if (valueType < Boolean || valueType > Map) {
        errors.append({path, "descriptor-type"});
        return d;
    }
    d.type = types[valueType];
    d.optional = optional;
    d.nullable = nullable;
    d.sensitive = sensitive;
    d.forbidden = forbidden;
    d.idField = idField;
    if (valueType == StringList)
        d.element = {ConfigDescriptor::leaf(T::String, QString())};
    else
        for (const auto *child : m_children) {
            auto descriptor = child->compile(errors, path + ConfigPath{child->key});
            if (d.type == T::List || d.type == T::Map)
                d.element.append(descriptor);
            else {
                if (child->key.isEmpty() || d.fields.contains(child->key))
                    errors.append({path, "duplicate-or-empty-key"});
                d.fields[child->key] = descriptor;
            }
        }
    if ((d.type == T::List || d.type == T::Map) && d.element.size() != 1) {
        errors.append({path, "element-descriptor"});
        return d;
    }
    auto unconstrained = d;
    unconstrained.forbidden = false;
    auto convert = [&](const QVariant &v) {
        auto result = admitQmlValue(unconstrained, v, path);
        errors.append(result.errors);
        return result.value;
    };
    if (defaultValue.isValid())
        d.defaultValue = convert(defaultValue);
    for (const auto &choice : choices)
        d.choices.append(convert(choice));
    if (minimum.isValid())
        d.minimum = convert(minimum);
    if (maximum.isValid())
        d.maximum = convert(maximum);
    return d;
}
} // namespace configqml
