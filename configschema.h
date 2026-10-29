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
ConfigValueResult captureValue(const ConfigValueInput &input, const ConfigPath &path = {},
                               const ConfigDiagnosticBuilder &diagnostics = {});
struct ConfigDescriptor {
    enum class Format { Native, Qml };
    enum class AdmissionPolicy { Inherit, Strict, Tolerant };
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
        SafeInteger,
        SecretRef
    };
    Type type = Type::String;
    bool optional = false;
    bool nullable = false;
    bool sensitive = false;
    bool machineLocal = false;
    AdmissionPolicy admissionPolicy = AdmissionPolicy::Inherit;
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
                                       bool childrenAdmitted,
                                       const ConfigDiagnosticBuilder &diagnostics) const;
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
struct ConfigMigration {
    int fromVersion = 0;
    // Worker-only, owned plain data; no application QObject or engine captures.
    std::function<ConfigValueResult(const QVariantMap &)> transform;
};
class ConfigSchema {
public:
    explicit ConfigSchema(const ConfigDescriptor &root, int version = 0,
                          QList<ConfigMigration> migrations = {});
    int version() const { return m_version; }
    QList<ConfigMigration> migrations() const { return m_migrations; }
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
    ConfigDescriptor::AdmissionPolicy policy(const ConfigPath &path) const;
    bool portable(const ConfigPath &path) const;
    bool sensitive(const ConfigPath &path) const;
    ConfigDiagnosticBuilder diagnostics() const { return ConfigDiagnosticBuilder(m_root); }

private:
    ConfigDescriptor m_root;
    int m_version = 0;
    QList<ConfigMigration> m_migrations;
    QVariantMap m_defaults;
    QList<ConfigPath> m_leaves;
    QList<ConfigDiagnostic> m_errors;
    std::shared_ptr<const char> m_identity = std::make_shared<const char>(0);
};
} // namespace configqml
