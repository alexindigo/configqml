// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configschema.h"
#include "confignumber.h"
#include <QUrl>
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
    if (type == Type::Integer)
        return a.toLongLong() < b.toLongLong();
    if (type == Type::UnsignedInteger)
        return a.toULongLong() < b.toULongLong();
    return a.toDouble() < b.toDouble();
}

static bool supportedConstraints(const ConfigDescriptor &d) {
    if (d.type < Type::Group || d.type > Type::Map)
        return false;
    const bool numeric =
        d.type == Type::Integer || d.type == Type::UnsignedInteger || d.type == Type::Real;
    const bool collection = d.type == Type::List || d.type == Type::Map;
    return !((!numeric && (d.minimum || d.maximum)) ||
             (collection ? d.element.size() != 1 : !d.element.isEmpty()) ||
             (d.type != Type::Record && d.type != Type::Group && !d.fields.isEmpty()) ||
             (d.type == Type::Group &&
              (d.defaultValue || d.optional || d.nullable || d.forbidden || !d.choices.isEmpty())));
}

static ConfigValueResult numericValue(const QVariant &input, Type type, const ConfigPath &path) {
    auto result = ownValue(input, path);
    if (result.ok() && result.value.metaType() == QMetaType::fromType<ConfigNumber>()) {
        const auto number = result.value.value<ConfigNumber>();
        if (type == Type::Integer || type == Type::UnsignedInteger)
            return number.integer(type == Type::UnsignedInteger, path);
        if (type == Type::Real)
            return number.real(path);
    }
    return result;
}

ConfigValueResult ConfigDescriptor::validate(const QVariant &input, const ConfigPath &path) const {
    auto owned = numericValue(input, type, path);
    if (!owned.ok())
        return owned;
    auto fail = [&](const char *code) {
        return ConfigValueResult{{}, {{path, QString::fromLatin1(code)}}};
    };
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
        matches = id == QMetaType::LongLong;
        break;
    case Type::UnsignedInteger:
        matches = id == QMetaType::ULongLong;
        break;
    case Type::Real:
        matches = id == QMetaType::Double;
        break;
    case Type::String:
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
    ConfigValueResult result = owned;
    if (!null && (type == Type::List || type == Type::Map)) {
        if (type == Type::List) {
            auto list = v.toList();
            for (qsizetype i = 0; i < list.size(); ++i) {
                auto checked =
                    element.first().validate(list[i], path + ConfigPath{QString::number(i)});
                result.errors.append(checked.errors);
                list[i] = checked.value;
            }
            result.value = list;
        } else {
            auto map = v.toMap();
            for (auto it = map.begin(); it != map.end(); ++it) {
                auto checked = element.first().validate(it.value(), path + ConfigPath{it.key()});
                result.errors.append(checked.errors);
                it.value() = checked.value;
            }
            result.value = map;
        }
    } else if (!null && (type == Type::Record || type == Type::Group)) {
        auto map = v.toMap();
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            const auto childPath = path + ConfigPath{it.key()};
            if (map.contains(it.key())) {
                auto checked = it->validate(map.value(it.key()), childPath);
                result.errors.append(checked.errors);
                map[it.key()] = checked.value;
            } else if (!it->optional && !it->forbidden)
                result.errors.append({childPath, "required"});
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
            const auto c = unconstrained.validate(choice, path);
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
            const auto b = numericValue(*bound, type, path);
            if (!b.ok() || b.value.metaType() != v.metaType())
                return fail("range-descriptor");
            if (lower ? less(v, b.value, type) : less(b.value, v, type))
                return fail("range");
        }
    }
    return result;
}

ConfigSchema::ConfigSchema(const ConfigDescriptor &root) : m_root(root) {
    if (root.type != Type::Group) {
        m_errors.append(ConfigDiagnostic{{}, "root-group"});
        return;
    }
    std::function<void(const ConfigDescriptor &, ConfigPath, bool)> inspect;
    inspect = [&](const ConfigDescriptor &d, ConfigPath path, bool setting) {
        if (path.size() > 64) {
            m_errors.append({path, "schema-depth"});
            return;
        }
        const bool numeric =
            d.type == Type::Integer || d.type == Type::UnsignedInteger || d.type == Type::Real;
        if (!supportedConstraints(d))
            m_errors.append({path, "descriptor"});
        auto unconstrained = d;
        unconstrained.choices.clear();
        unconstrained.minimum.reset();
        unconstrained.maximum.reset();
        unconstrained.forbidden = false;
        for (const auto &c : d.choices)
            m_errors.append(unconstrained.validate(c, path).errors);
        for (const auto &b : {d.minimum, d.maximum})
            if (b) {
                m_errors.append(unconstrained.validate(*b, path).errors);
                if (isNull(*b))
                    m_errors.append({path, "range-descriptor"});
            }
        if (d.minimum && d.maximum && numeric) {
            const auto low = numericValue(*d.minimum, d.type, path);
            const auto high = numericValue(*d.maximum, d.type, path);
            if (low.ok() && high.ok() && less(high.value, low.value, d.type))
                m_errors.append({path, "range-order"});
        }
        if (d.defaultValue) {
            const auto checked = d.validate(*d.defaultValue, path);
            m_errors.append(checked.errors);
            if (setting && checked.ok())
                putValue(m_defaults, path, checked.value);
        } else if (setting && d.type != Type::Group && !d.optional && !d.forbidden) {
            m_errors.append({path, "required-default"});
        }
        if (setting && d.type != Type::Group)
            m_leaves.append(path);
        for (auto it = d.fields.cbegin(); it != d.fields.cend(); ++it) {
            if (it.key().isEmpty())
                m_errors.append({path, "empty-key"});
            inspect(it.value(), path + ConfigPath{it.key()}, setting && d.type == Type::Group);
        }
        for (const auto &item : d.element) {
            if (item.optional)
                m_errors.append({path, "optional-element"});
            inspect(item, path + ConfigPath{"*"}, false);
        }
    };
    inspect(root, {}, true);
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
ConfigValueResult ConfigSchema::validate(const ConfigPath &path, const QVariant &value) const {
    if (!valid())
        return {{}, {{{}, "invalid-schema"}}};
    const auto d = descriptor(path);
    if (!d || d->type == Type::Group)
        return {{}, {{path, "undeclared-leaf"}}};
    return d->validate(value, path);
}
} // namespace configqml
