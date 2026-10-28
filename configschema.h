// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configvalue.h"

namespace configqml {
struct ConfigDescriptor {
    enum class Type {
        Group,
        Boolean,
        Integer,
        UnsignedInteger,
        Real,
        String,
        Url,
        List,
        Record,
        Map,
        SafeInteger
    };
    Type type = Type::String;
    bool optional = false;
    bool nullable = false;
    bool sensitive = false;
    bool forbidden = false;
    std::optional<QVariant> defaultValue;
    QList<QVariant> choices;
    std::optional<QVariant> minimum;
    std::optional<QVariant> maximum;
    QMap<QString, ConfigDescriptor> fields; // Group/Record children.
    QList<ConfigDescriptor> element; // Exactly one for List/Map; value-owned recursive descriptor.

    static ConfigDescriptor group(const QMap<QString, ConfigDescriptor> &fields);
    static ConfigDescriptor leaf(Type type, const QVariant &defaultValue);
    static ConfigDescriptor list(const ConfigDescriptor &element, const QVariantList &defaultValue);
    ConfigValueResult validate(const QVariant &value, const ConfigPath &path = {}) const;
};

class ConfigSchema {
public:
    explicit ConfigSchema(const ConfigDescriptor &root);
    bool valid() const { return m_errors.isEmpty(); }
    QList<ConfigDiagnostic> errors() const { return m_errors; }
    QVariantMap defaults() const { return m_defaults; }
    std::optional<ConfigDescriptor> descriptor(const ConfigPath &path) const;
    QList<ConfigPath> leaves() const { return m_leaves; }
    ConfigValueResult validate(const ConfigPath &path, const QVariant &value) const;

private:
    ConfigDescriptor m_root;
    QVariantMap m_defaults;
    QList<ConfigPath> m_leaves;
    QList<ConfigDiagnostic> m_errors;
};
} // namespace configqml
