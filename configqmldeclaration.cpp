// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmldeclaration.h"
#include "configqmlvalue.h"
namespace configqml {
ConfigDescriptor ConfigQmlGroup::compile(QList<ConfigDiagnostic> &errors,
                                         const ConfigPath &path) const {
    const auto first = errors.size();
    QMap<QString, ConfigDescriptor> fields;
    for (const auto *child : m_children) {
        if (child->key.isEmpty() || fields.contains(child->key))
            errors.append(ConfigDiagnosticBuilder().make(path, "duplicate-or-empty-key"));
        fields[child->key] = child->compile(errors, path + ConfigPath{child->key});
    }
    auto descriptor = ConfigDescriptor::group(fields);
    descriptor.sensitive = sensitive;
    descriptor.machineLocal = machineLocal;
    descriptor.admissionPolicy = ConfigDescriptor::AdmissionPolicy(admissionPolicy);
    const ConfigDiagnosticBuilder diagnostics(descriptor, path);
    for (qsizetype i = first; i < errors.size(); ++i)
        errors[i] = diagnostics.make(errors[i].path, errors[i].code);
    return descriptor;
}
ConfigDescriptor ConfigQmlValue::compile(QList<ConfigDiagnostic> &errors,
                                         const ConfigPath &path) const {
    using T = ConfigDescriptor::Type;
    static const T types[] = {T::Boolean, T::SafeInteger, T::Integer, T::UnsignedInteger,
                              T::Real,    T::String,      T::Url,     T::List,
                              T::List,    T::Record,      T::Map,     T::SecretRef};
    ConfigDescriptor d;
    if (valueType < Boolean || valueType > SecretRef) {
        errors.append(ConfigDiagnosticBuilder().make(path, "descriptor-type"));
        return d;
    }
    d.type = types[valueType];
    d.optional = optional;
    d.nullable = nullable;
    d.sensitive = sensitive;
    d.machineLocal = machineLocal;
    d.admissionPolicy = ConfigDescriptor::AdmissionPolicy(admissionPolicy);
    d.forbidden = forbidden;
    d.idField = idField;
    d.stringPattern = ownString(stringPattern);
    if (valueType == StringList)
        d.element = {ConfigDescriptor::leaf(T::String, QString())};
    else
        for (const auto *child : m_children) {
            auto descriptor = child->compile(errors, path + ConfigPath{child->key});
            if (d.type == T::List || d.type == T::Map)
                d.element.append(descriptor);
            else {
                if (child->key.isEmpty() || d.fields.contains(child->key))
                    errors.append(
                        ConfigDiagnosticBuilder(d, path).make(path, "duplicate-or-empty-key"));
                d.fields[child->key] = descriptor;
            }
        }
    if ((d.type == T::List || d.type == T::Map) && d.element.size() != 1) {
        errors.append(ConfigDiagnosticBuilder(d, path).make(path, "element-descriptor"));
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
