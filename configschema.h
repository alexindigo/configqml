// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configvalue.h"
#include <functional>
#include <memory>

namespace configqml {
// A synchronous, non-retained view of input. Core owns/captures each visited
// value before returning; an engine adapter never crosses the worker boundary.
class ConfigValueInput {
public:
    enum class Kind { Scalar, List, Map };
    using Visitor = std::function<void(const QString &, const ConfigValueInput &)>;
    virtual ~ConfigValueInput() = default;
    virtual Kind kind() const = 0;
    virtual QVariant scalar() const = 0;
    virtual qsizetype count() const = 0;
    virtual QString error() const { return {}; }
    virtual void visit(const Visitor &visitor) const = 0;
};
struct ConfigCapture;
ConfigValueResult captureValue(const ConfigValueInput &input, const ConfigPath &path = {});
struct ConfigDescriptor {
    enum class Format { Native, Qml };
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
    QString idField; // Optional required scalar record field selecting list row identity.

    static ConfigDescriptor group(const QMap<QString, ConfigDescriptor> &fields);
    static ConfigDescriptor leaf(Type type, const QVariant &defaultValue);
    static ConfigDescriptor list(const ConfigDescriptor &element, const QVariantList &defaultValue);
    ConfigValueResult validate(const QVariant &value, const ConfigPath &path = {},
                               Format format = Format::Native) const;
    ConfigValueResult validate(const ConfigValueInput &input, const ConfigPath &path = {},
                               Format format = Format::Native) const;

private:
    friend struct ConfigCapture;
    friend class ConfigSchema;
    ConfigValueResult validateCaptured(const QVariant &value, const ConfigPath &path, Format format,
                                       bool childrenAdmitted = false) const;
};

// Sealed result of an owned schema admission. No public unchecked-value constructor.
class ConfigAdmission {
public:
    ConfigAdmission() = default;
    bool ok() const { return m_identity && m_result.ok(); }
    ConfigPath path() const { return m_path; }
    QVariant value() const { return m_result.value; }
    QList<ConfigDiagnostic> errors() const { return m_result.errors; }

private:
    friend class ConfigSchema;
    ConfigPath m_path;
    ConfigValueResult m_result;
    std::shared_ptr<const char> m_identity;
};
class ConfigSchema {
public:
    explicit ConfigSchema(const ConfigDescriptor &root);
    bool valid() const { return m_errors.isEmpty(); }
    QList<ConfigDiagnostic> errors() const { return m_errors; }
    QVariantMap defaults() const { return m_defaults; }
    std::optional<ConfigDescriptor> descriptor(const ConfigPath &path) const;
    QList<ConfigPath> leaves() const { return m_leaves; }
    ConfigValueResult
    validate(const ConfigPath &path, const QVariant &value,
             ConfigDescriptor::Format format = ConfigDescriptor::Format::Native) const;
    ConfigAdmission admit(const ConfigPath &path, const ConfigValueInput &input,
                          ConfigDescriptor::Format format = ConfigDescriptor::Format::Native) const;
    ConfigValueResult admitted(const ConfigAdmission &input) const;

private:
    ConfigDescriptor m_root;
    QVariantMap m_defaults;
    QList<ConfigPath> m_leaves;
    QList<ConfigDiagnostic> m_errors;
    std::shared_ptr<const char> m_identity = std::make_shared<const char>(0);
};
} // namespace configqml
