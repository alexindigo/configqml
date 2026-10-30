// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configschema.h"
#include "confignumber.h"
#include <QUrl>
#include <QSet>
#include <cmath>
#include <functional>

namespace configqml {
using Type = ConfigDescriptor::Type;
ConfigDescriptor ConfigDescriptor::group(const QMap<QString, ConfigDescriptor> &fields) {
    ConfigDescriptor d;
    d.type = Type::Group;
    d.fields = fields;
    return d;
}
ConfigDescriptor ConfigDescriptor::leaf(Type type, const QVariant &value) {
    ConfigDescriptor d;
    d.type = type;
    d.defaultValue = value;
    return d;
}
ConfigDescriptor ConfigDescriptor::list(const ConfigDescriptor &element,
                                        const QVariantList &value) {
    auto d = leaf(Type::List, value);
    d.element = {element};
    return d;
}

// Compare normalized numeric values in their declared domain, never via double for integers.
static bool less(const QVariant &a, const QVariant &b, Type type) {
    if (type == Type::Integer || type == Type::SafeInteger)
        return a.toLongLong() < b.toLongLong();
    if (type == Type::UnsignedInteger)
        return a.toULongLong() < b.toULongLong();
    return a.toDouble() < b.toDouble();
}

static bool supportedConstraints(const ConfigDescriptor &d) {
    if (d.type < Type::Group || d.type > Type::SecretRef ||
        d.admissionPolicy < ConfigDescriptor::AdmissionPolicy::Inherit ||
        d.admissionPolicy > ConfigDescriptor::AdmissionPolicy::Tolerant)
        return false;
    const bool numeric = d.type == Type::Integer || d.type == Type::SafeInteger ||
                         d.type == Type::UnsignedInteger || d.type == Type::Real;
    const bool collection = d.type == Type::List || d.type == Type::Map;
    if (!d.idField.isEmpty()) {
        if (d.type != Type::List || d.element.size() != 1 ||
            d.element.first().type != Type::Record || d.element.first().nullable ||
            !d.element.first().fields.contains(d.idField))
            return false;
        const auto field = d.element.first().fields.value(d.idField);
        if (field.optional || field.nullable || field.forbidden ||
            (field.type != Type::String && field.type != Type::Integer &&
             field.type != Type::UnsignedInteger && field.type != Type::SafeInteger))
            return false;
    }
    if (collection && d.element.size() == 1 && d.element.first().type == Type::Record)
        for (const auto &name : {QStringLiteral("modelData"), QStringLiteral("rowId")})
            if (d.element.first().fields.contains(name))
                return false;
    return !((!numeric && (d.minimum || d.maximum)) ||
             (collection ? d.element.size() != 1 : !d.element.isEmpty()) ||
             (d.type != Type::Record && d.type != Type::Group && !d.fields.isEmpty()) ||
             (d.type == Type::Group &&
              (d.defaultValue || d.optional || d.nullable || d.forbidden || !d.choices.isEmpty())));
}

static ConfigValueResult numericValue(const QVariant &input, Type type, const ConfigPath &path,
                                      const ConfigDiagnosticBuilder &diagnostics) {
    ConfigValueResult result{input, {}};
    // Scalar normalization only. The owned tree is captured once by validate(),
    // never recaptured recursively for every descriptor level.
    switch (input.metaType().id()) {
    case QMetaType::Int:
        result.value = QVariant::fromValue(input.toLongLong());
        break;
    case QMetaType::UInt:
        result.value = QVariant::fromValue(input.toULongLong());
        break;
    case QMetaType::Float:
        result.value = input.toDouble();
        [[fallthrough]];
    case QMetaType::Double:
        if (!std::isfinite(input.toDouble()))
            return {{}, {diagnostics.make(path, "non-finite")}};
        break;
    default:
        break;
    }
    if (result.ok() && result.value.metaType() == QMetaType::fromType<ConfigNumber>()) {
        const auto number = result.value.value<ConfigNumber>();
        if (type == Type::Integer || type == Type::SafeInteger || type == Type::UnsignedInteger)
            return number.integer(type == Type::UnsignedInteger, path, diagnostics);
        if (type == Type::Real)
            return number.real(path, diagnostics);
    }
    return result;
}

static ConfigValueResult validateNative(const ConfigDescriptor &, const QVariant &,
                                        const ConfigPath &, ConfigDescriptor::Format,
                                        bool ownedDescriptor = false,
                                        const ConfigDiagnosticBuilder *diagnostics = nullptr);
ConfigValueResult ConfigDescriptor::validate(const QVariant &input, const ConfigPath &path,
                                             Format format) const {
    return validateNative(*this, input, path, format);
}
ConfigValueResult
ConfigDescriptor::validateCaptured(const QVariant &input, const ConfigPath &path, Format format,
                                   bool childrenAdmitted,
                                   const ConfigDiagnosticBuilder &diagnostics) const {
    auto owned = numericValue(input, type, path, diagnostics);
    if (!owned.ok())
        return owned;
    auto fail = [&](const char *code) {
        return ConfigValueResult{{}, {diagnostics.make(path, QString::fromLatin1(code))}};
    };
    if (format == Format::Qml && !isNull(input)) {
        if (type == Type::Integer || type == Type::UnsignedInteger) {
            if (input.metaType().id() != QMetaType::QString)
                return fail("integer64-requires-decimal-string");
            const auto text = input.toString();
            const bool negative = text.startsWith('-');
            const int start = negative ? 1 : 0;
            if (text.size() <= start || (negative && type == Type::UnsignedInteger) ||
                (text.size() > start + 1 && text[start] == QChar('0')))
                return fail("integer64-decimal-syntax");
            for (int i = start; i < text.size(); ++i)
                if (text[i] < QChar('0') || text[i] > QChar('9'))
                    return fail("integer64-decimal-syntax");
            bool ok;
            owned.value = type == Type::UnsignedInteger ? QVariant::fromValue(text.toULongLong(&ok))
                                                        : QVariant::fromValue(text.toLongLong(&ok));
            if (!ok)
                return fail("integer-range");
        } else if (type == Type::SafeInteger && input.metaType().id() == QMetaType::Double) {
            const auto n = input.toDouble();
            if (std::trunc(n) != n)
                return fail("fractional-integer");
            if (n < -9007199254740991.0 || n > 9007199254740991.0)
                return fail("unsafe-integer");
            owned.value = QVariant::fromValue(qint64(n));
        } else if (type == Type::Real && input.metaType().id() == QMetaType::LongLong)
            owned.value = input.toDouble();
    }
    if (!supportedConstraints(*this))
        return fail("descriptor");
    if (forbidden)
        return fail("forbidden");
    const bool null = isNull(owned.value);
    if (null && !nullable)
        return fail("not-nullable");
    const auto &v = owned.value;
    const int id = v.metaType().id();
    bool matches = false;
    switch (type) {
    case Type::Boolean:
        matches = id == QMetaType::Bool;
        break;
    case Type::Integer:
    case Type::SafeInteger:
        matches = id == QMetaType::LongLong;
        break;
    case Type::UnsignedInteger:
        matches = id == QMetaType::ULongLong;
        break;
    case Type::Real:
        matches = id == QMetaType::Double;
        break;
    case Type::String:
    case Type::SecretRef:
        matches = id == QMetaType::QString;
        break;
    case Type::Url:
        matches = id == QMetaType::QUrl;
        break;
    case Type::List:
        matches = id == QMetaType::QVariantList;
        break;
    case Type::Group:
    case Type::Record:
    case Type::Map:
        matches = id == QMetaType::QVariantMap;
        break;
    }
    if (!matches && !null)
        return fail("type");
    if (!null && type == Type::SafeInteger &&
        (v.toLongLong() < -9007199254740991LL || v.toLongLong() > 9007199254740991LL))
        return fail("unsafe-integer");
    ConfigValueResult result = owned;
    if (!childrenAdmitted && !null && (type == Type::List || type == Type::Map)) {
        if (type == Type::List) {
            auto list = v.toList();
            QSet<QString> identities;
            for (qsizetype i = 0; i < list.size(); ++i) {
                auto checked = element.first().validateCaptured(
                    list[i], path + ConfigPath{QString::number(i)}, format, false, diagnostics);
                result.errors.append(checked.errors);
                list[i] = checked.value;
                if (checked.ok() && !idField.isEmpty()) {
                    const auto key = checked.value.toMap().value(idField).toString();
                    if (identities.contains(key))
                        result.errors.append(diagnostics.make(
                            path + ConfigPath{QString::number(i), idField}, "duplicate-row-id"));
                    identities.insert(key);
                }
            }
            result.value = list;
        } else {
            auto map = v.toMap();
            for (auto it = map.begin(); it != map.end(); ++it) {
                auto checked = element.first().validateCaptured(
                    it.value(), path + ConfigPath{it.key()}, format, false, diagnostics);
                result.errors.append(checked.errors);
                it.value() = checked.value;
            }
            result.value = map;
        }
    } else if (!childrenAdmitted && !null && (type == Type::Record || type == Type::Group)) {
        auto map = v.toMap();
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            const auto childPath = path + ConfigPath{it.key()};
            if (map.contains(it.key())) {
                auto checked = it->validateCaptured(map.value(it.key()), childPath, format, false,
                                                    diagnostics);
                result.errors.append(checked.errors);
                map[it.key()] = checked.value;
            } else if (!it->optional && !it->forbidden)
                result.errors.append(diagnostics.make(childPath, "required"));
        }
        result.value = map;
    }
    if (!result.ok()) {
        result.value.clear();
        return result;
    }
    if (!choices.isEmpty()) {
        auto unconstrained = *this;
        unconstrained.choices.clear();
        unconstrained.minimum.reset();
        unconstrained.maximum.reset();
        bool found = false;
        for (const auto &choice : choices) {
            const auto c =
                unconstrained.validateCaptured(choice, path, Format::Native, false, diagnostics);
            found |=
                c.ok() && c.value.metaType() == result.value.metaType() && c.value == result.value;
        }
        if (!found)
            return fail("choice");
    }
    if (!null) {
        for (bool lower : {true, false}) {
            const auto bound = lower ? minimum : maximum;
            if (!bound)
                continue;
            const auto b = numericValue(*bound, type, path, diagnostics);
            if (!b.ok() || b.value.metaType() != v.metaType())
                return fail("range-descriptor");
            if (lower ? less(v, b.value, type) : less(b.value, v, type))
                return fail("range");
        }
    }
    return result;
}

namespace {
class NativeInput final : public ConfigValueInput {
public:
    explicit NativeInput(const QVariant &value) : m_value(value) {
        const auto type = value.metaType().id();
        if (type == QMetaType::QVariantList || type == QMetaType::QStringList) {
            m_kind = Kind::List;
            m_list = value.toList();
        } else if (type == QMetaType::QVariantMap) {
            m_kind = Kind::Map;
            m_map = value.toMap();
        }
    }
    Kind kind() const override { return m_kind; }
    QVariant scalar() const override { return m_value; }
    qsizetype count() const override { return m_kind == Kind::List ? m_list.size() : m_map.size(); }
    void visit(const Visitor &visitor) const override {
        if (m_kind == Kind::List) {
            for (int i = 0; i < m_list.size(); ++i) {
                NativeInput child(m_list[i]);
                visitor(QString::number(i), child);
            }
        } else {
            for (auto it = m_map.cbegin(); it != m_map.cend(); ++it) {
                NativeInput child(it.value());
                visitor(it.key(), child);
            }
        }
    }

private:
    Kind m_kind = Kind::Scalar;
    QVariant m_value;
    QVariantList m_list;
    QVariantMap m_map;
};
} // namespace
struct ConfigCapture {
    static ConfigValueResult read(const ConfigDescriptor *d, const ConfigValueInput &input,
                                  ConfigPath &path, ConfigDescriptor::Format format, int depth,
                                  bool ownedDescriptor,
                                  const ConfigDiagnosticBuilder &diagnostics) {
        using F = ConfigDescriptor::Format;
        using K = ConfigValueInput::Kind;
        auto fail = [&](const QString &code) {
            return ConfigValueResult{{}, {diagnostics.make(path, code)}};
        };
        if (depth > 64)
            return fail("value-depth");
        const auto inputError = input.error();
        if (!inputError.isEmpty())
            return fail(inputError);
        if (d && d->forbidden)
            return fail("forbidden");
        const auto kind = input.kind();
        if (kind == K::Scalar) {
            const auto raw = input.scalar();
            const auto rawType = raw.metaType().id();
            if (rawType == QMetaType::QVariantList || rawType == QMetaType::QStringList ||
                rawType == QMetaType::QVariantMap)
                return fail("input-kind");
            // Decimal strings become numeric primitives; no intermediate owned
            // string/tree is retained. All other leaves cross ownValue once.
            if (d && format == F::Qml && raw.isValid() &&
                (d->type == Type::Integer || d->type == Type::UnsignedInteger))
                return d->validateCaptured(raw, path, format, true, diagnostics);
            const auto captured = ownValue(raw, path, diagnostics);
            return d && captured.ok()
                       ? d->validateCaptured(captured.value, path, format, true, diagnostics)
                       : captured;
        }
        if (d) {
            if (kind == K::List && d->type != Type::List)
                return fail("type");
            if (kind == K::Map && d->type != Type::Map && d->type != Type::Record &&
                d->type != Type::Group)
                return fail("type");
            if ((d->type == Type::List || d->type == Type::Map) && d->element.size() != 1)
                return fail("descriptor");
        }
        QVariantList list;
        QVariantMap map;
        if (kind == K::List)
            list.reserve(input.count());
        QSet<QString> identities;
        ConfigValueResult result;
        input.visit([&](const QString &inputKey, const ConfigValueInput &child) {
            const ConfigDescriptor *childDescriptor = nullptr;
            QString key;
            if (d && (d->type == Type::List || d->type == Type::Map))
                childDescriptor = &d->element.first();
            else if (d) {
                auto field = d->fields.constFind(inputKey);
                if (field != d->fields.cend()) {
                    childDescriptor = &field.value();
                    if (ownedDescriptor)
                        key = field.key();
                }
            }
            if (key.isNull())
                key = ownString(inputKey);
            path.append(key);
            auto value =
                read(childDescriptor, child, path, format, depth + 1, ownedDescriptor, diagnostics);
            result.errors.append(value.errors);
            if (kind == K::List) {
                if (value.ok() && d && !d->idField.isEmpty()) {
                    const auto id = value.value.toMap().value(d->idField).toString();
                    if (identities.contains(id))
                        result.errors.append(
                            diagnostics.make(path + ConfigPath{d->idField}, "duplicate-row-id"));
                    identities.insert(id);
                }
                list.append(value.value);
            } else
                map.insert(key, value.value);
            path.removeLast();
        });
        if (d && (d->type == Type::Record || d->type == Type::Group))
            for (auto field = d->fields.cbegin(); field != d->fields.cend(); ++field)
                if (!map.contains(field.key()) && !field->optional && !field->forbidden)
                    result.errors.append(
                        diagnostics.make(path + ConfigPath{field.key()}, "required"));
        if (!result.ok())
            return result;
        const QVariant value = kind == K::List ? QVariant(list) : QVariant(map);
        return d ? d->validateCaptured(value, path, format, true, diagnostics)
                 : ConfigValueResult{value, {}};
    }
};
static ConfigValueResult validateNative(const ConfigDescriptor &d, const QVariant &value,
                                        const ConfigPath &path, ConfigDescriptor::Format format,
                                        bool ownedDescriptor,
                                        const ConfigDiagnosticBuilder *diagnostics) {
    NativeInput input(value);
    auto ownedPath = ownPath(path);
    const auto bound = diagnostics ? *diagnostics : ConfigDiagnosticBuilder(d, path);
    return ConfigCapture::read(&d, input, ownedPath, format, 0, ownedDescriptor, bound);
}
ConfigValueResult captureValue(const ConfigValueInput &input, const ConfigPath &path,
                               const ConfigDiagnosticBuilder &diagnostics) {
    auto ownedPath = ownPath(path);
    return ConfigCapture::read(nullptr, input, ownedPath, ConfigDescriptor::Format::Native, 0,
                               false, diagnostics);
}
ConfigValueResult ConfigDescriptor::validate(const ConfigValueInput &input, const ConfigPath &path,
                                             Format format) const {
    auto ownedPath = ownPath(path);
    return ConfigCapture::read(this, input, ownedPath, format, 0, false,
                               ConfigDiagnosticBuilder(*this, path));
}

static ConfigDescriptor ownDescriptor(const ConfigDescriptor &input, const ConfigPath &path,
                                      QList<ConfigDiagnostic> &errors,
                                      const ConfigDiagnosticBuilder &diagnostics) {
    auto out = input;
    out.idField = QString(input.idField.constData(), input.idField.size());
    out.fields.clear();
    out.element.clear();
    out.choices.clear();
    if (path.size() > 64) {
        errors.append(diagnostics.make(path, "schema-depth"));
        return out;
    }
    auto capture = [&](const QVariant &v) {
        auto result = ownValue(v, path, diagnostics);
        errors.append(result.errors);
        return result.value;
    };
    if (input.defaultValue)
        out.defaultValue = capture(*input.defaultValue);
    if (input.minimum)
        out.minimum = capture(*input.minimum);
    if (input.maximum)
        out.maximum = capture(*input.maximum);
    for (const auto &choice : input.choices)
        out.choices.append(capture(choice));
    for (auto it = input.fields.cbegin(); it != input.fields.cend(); ++it) {
        const QString key(it.key().constData(), it.key().size());
        out.fields.insert(key,
                          ownDescriptor(it.value(), path + ConfigPath{key}, errors, diagnostics));
    }
    for (const auto &element : input.element)
        out.element.append(ownDescriptor(element, path + ConfigPath{"*"}, errors, diagnostics));
    return out;
}

ConfigSchema::ConfigSchema(const ConfigDescriptor &root, int version,
                           QList<ConfigMigration> migrations)
    : m_version(version), m_migrations(std::move(migrations)) {
    const ConfigDiagnosticBuilder diagnostics(root);
    if (version < 0)
        m_errors.append(diagnostics.make({}, "schema-version"));
    QSet<int> versions;
    for (const auto &migration : m_migrations) {
        if (migration.fromVersion < 0 || migration.fromVersion >= version || !migration.transform ||
            versions.contains(migration.fromVersion))
            m_errors.append(diagnostics.make({}, "migration-descriptor"));
        versions.insert(migration.fromVersion);
    }
    m_root = ownDescriptor(root, {}, m_errors, diagnostics);
    if (root.type != Type::Group) {
        m_errors.append(diagnostics.make({}, "root-group"));
        return;
    }
    std::function<void(const ConfigDescriptor &, ConfigPath, bool)> inspect;
    inspect = [&](const ConfigDescriptor &d, ConfigPath path, bool setting) {
        if (path.size() > 64) {
            m_errors.append(diagnostics.make(path, "schema-depth"));
            return;
        }
        const bool numeric = d.type == Type::Integer || d.type == Type::SafeInteger ||
                             d.type == Type::UnsignedInteger || d.type == Type::Real;
        if (!supportedConstraints(d))
            m_errors.append(diagnostics.make(path, "descriptor"));
        auto unconstrained = d;
        unconstrained.choices.clear();
        unconstrained.minimum.reset();
        unconstrained.maximum.reset();
        unconstrained.forbidden = false;
        for (const auto &c : d.choices)
            m_errors.append(validateNative(unconstrained, c, path, ConfigDescriptor::Format::Native,
                                           true, &diagnostics)
                                .errors);
        for (const auto &b : {d.minimum, d.maximum})
            if (b) {
                m_errors.append(validateNative(unconstrained, *b, path,
                                               ConfigDescriptor::Format::Native, true, &diagnostics)
                                    .errors);
                if (isNull(*b))
                    m_errors.append(diagnostics.make(path, "range-descriptor"));
            }
        if (d.minimum && d.maximum && numeric) {
            const auto low = numericValue(*d.minimum, d.type, path, diagnostics);
            const auto high = numericValue(*d.maximum, d.type, path, diagnostics);
            if (low.ok() && high.ok() && less(high.value, low.value, d.type))
                m_errors.append(diagnostics.make(path, "range-order"));
        }
        if (d.defaultValue) {
            const auto checked = validateNative(
                d, *d.defaultValue, path, ConfigDescriptor::Format::Native, true, &diagnostics);
            m_errors.append(checked.errors);
            if (setting && checked.ok())
                putValue(m_defaults, path, checked.value);
        } else if (setting && d.type != Type::Group && !d.optional && !d.forbidden) {
            m_errors.append(diagnostics.make(path, "required-default"));
        }
        if (setting && d.type != Type::Group)
            m_leaves.append(path);
        for (auto it = d.fields.cbegin(); it != d.fields.cend(); ++it) {
            if (it.key().isEmpty() || it.key() == "_configqmlVersion")
                m_errors.append(diagnostics.make(path, "empty-key"));
            inspect(it.value(), path + ConfigPath{it.key()}, setting && d.type == Type::Group);
        }
        for (const auto &item : d.element) {
            if (item.optional)
                m_errors.append(diagnostics.make(path, "optional-element"));
            inspect(item, path + ConfigPath{"*"}, false);
        }
    };
    inspect(m_root, {}, true);
    if (!valid())
        m_defaults.clear();
}

std::optional<ConfigDescriptor> ConfigSchema::descriptor(const ConfigPath &path) const {
    auto d = m_root;
    for (const auto &key : path) {
        if (d.type != Type::Group || !d.fields.contains(key))
            return std::nullopt;
        d = d.fields.value(key);
    }
    return d;
}
ConfigValueResult ConfigSchema::validate(const ConfigPath &path, const QVariant &value,
                                         ConfigDescriptor::Format format) const {
    const auto builder = diagnostics();
    if (!valid())
        return {{}, {builder.make({}, "invalid-schema")}};
    const auto d = descriptor(path);
    if (!d || d->type == Type::Group)
        return {{}, {builder.make(path, "undeclared-leaf")}};
    return validateNative(*d, value, path, format, true, &builder);
}
ConfigAdmission ConfigSchema::admit(const ConfigPath &path, const ConfigValueInput &input,
                                    ConfigDescriptor::Format format) const {
    ConfigAdmission result;
    result.m_path = ownPath(path);
    result.m_identity = m_identity;
    const auto d = descriptor(path);
    if (!valid())
        result.m_result.errors = {diagnostics().make({}, "invalid-schema")};
    else if (!d || d->type == Type::Group)
        result.m_result.errors = {diagnostics().make(result.m_path, "undeclared-leaf")};
    else
        result.m_result =
            ConfigCapture::read(&*d, input, result.m_path, format, 0, true, diagnostics());
    return result;
}
ConfigValueResult ConfigSchema::admitted(const ConfigAdmission &input) const {
    if (!input.m_identity || !m_identity || input.m_identity != m_identity)
        return {{}, {diagnostics().make(input.m_path, "schema-mismatch")}};
    return input.m_result;
}
ConfigDescriptor::AdmissionPolicy ConfigSchema::policy(const ConfigPath &path) const {
    auto node = m_root;
    auto result = node.admissionPolicy;
    for (const auto &key : path) {
        if ((node.type == Type::List || node.type == Type::Map) && node.element.size() == 1)
            node = node.element.first();
        else {
            if (!node.fields.contains(key))
                break;
            node = node.fields.value(key);
        }
        if (node.admissionPolicy != ConfigDescriptor::AdmissionPolicy::Inherit)
            result = node.admissionPolicy;
    }
    return result;
}
static bool excluded(const ConfigDescriptor &node) {
    if (node.sensitive || node.machineLocal || node.forbidden ||
        node.type == ConfigDescriptor::Type::SecretRef)
        return true;
    for (const auto &child : node.fields)
        if (excluded(child))
            return true;
    for (const auto &child : node.element)
        if (excluded(child))
            return true;
    return false;
}
bool ConfigSchema::portable(const ConfigPath &path) const {
    auto node = m_root;
    if (node.sensitive || node.machineLocal || node.forbidden)
        return false;
    for (const auto &key : path) {
        if (node.type != Type::Group || !node.fields.contains(key))
            return false;
        node = node.fields.value(key);
        if (node.sensitive || node.machineLocal || node.forbidden)
            return false;
    }
    return node.type != Type::Group && !excluded(node);
}
static bool hasSensitive(const ConfigDescriptor &node) {
    if (node.sensitive || node.type == Type::SecretRef)
        return true;
    for (const auto &child : node.fields)
        if (hasSensitive(child))
            return true;
    for (const auto &child : node.element)
        if (hasSensitive(child))
            return true;
    return false;
}
bool ConfigSchema::sensitive(const ConfigPath &path) const {
    auto node = m_root;
    if (node.sensitive)
        return true;
    for (const auto &key : path) {
        if (node.type != Type::Group || !node.fields.contains(key))
            return false;
        node = node.fields.value(key);
        if (node.sensitive)
            return true;
    }
    return hasSensitive(node);
}
} // namespace configqml
