// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configrecordlistpolicy.h"
#include <QSet>
#include <functional>

namespace configqml {
namespace {
using Selector = ConfigValueSelector;
using Selectors = QList<Selector>;
bool mapValue(const QVariant &value) { return value.metaType().id() == QMetaType::QVariantMap; }
bool listValue(const QVariant &value) { return value.metaType().id() == QMetaType::QVariantList; }
QVariantList normalize(const ConfigRecordListPolicySpec &policy,
                       const ConfigRecordListMetadata &metadata, const QVariantList &raw,
                       QList<qsizetype> &mapping, QString &error) {
    QMap<QString, ConfigRecordListEntry> entries;
    for (const auto &entry : metadata.entries)
        entries.insert(entry.key, entry);
    QVariantList result;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (!mapValue(raw[i])) {
            error = "list-policy-value";
            return {};
        }
        auto row = raw[i].toMap();
        const auto key = row.value(policy.keyField);
        if (key.metaType().id() != QMetaType::QString) {
            error = "list-policy-value";
            return {};
        }
        const auto entry = entries.constFind(key.toString());
        if (metadata.ready && policy.pruneUnknown &&
            (entry == entries.cend() || (!entry->available && !entry->retain)))
            continue;
        if (metadata.ready && policy.normalizeFields && entry != entries.cend() &&
            entry->normalize) {
            const auto preserved = [&](const QString &field) {
                return field == policy.keyField || policy.preservedFields.contains(field);
            };
            for (auto it = row.begin(); it != row.end();)
                if (!preserved(it.key()) && !entry->defaults.contains(it.key()))
                    it = row.erase(it);
                else
                    ++it;
            for (auto it = entry->defaults.cbegin(); it != entry->defaults.cend(); ++it)
                if (!preserved(it.key()) && !row.contains(it.key()))
                    row.insert(it.key(), it.value());
        }
        mapping.append(i);
        result.append(row);
    }
    return result;
}
QVariant project(const ConfigRecordListPolicySpec &policy, const ConfigRecordListMetadata &metadata,
                 const QVariant &value, qsizetype depth, QList<qsizetype> address,
                 ConfigRecordListProjection &out) {
    if (!out.error.isEmpty())
        return {};
    if (depth == policy.listPath.size()) {
        if (!listValue(value)) {
            out.error = "list-policy-value";
            return {};
        }
        QList<qsizetype> mapping;
        auto result = normalize(policy, metadata, value.toList(), mapping, out.error);
        out.rows.insert(address, mapping);
        return result;
    }
    const auto selector = policy.listPath[depth];
    if (selector.kind == Selector::Kind::Key && mapValue(value)) {
        auto map = value.toMap();
        if (map.contains(selector.key))
            map[selector.key] =
                project(policy, metadata, map.value(selector.key), depth + 1, address, out);
        return map; // Missing optional lists remain missing.
    }
    if (selector.kind != Selector::Kind::Key && listValue(value)) {
        auto list = value.toList();
        if (selector.kind == Selector::Kind::Index) {
            if (selector.index < 0 || selector.index >= list.size()) {
                out.error = "list-policy-value";
                return {};
            }
            list[selector.index] = project(policy, metadata, list[selector.index], depth + 1,
                                           address + QList<qsizetype>{selector.index}, out);
        } else
            for (qsizetype i = 0; i < list.size(); ++i)
                list[i] = project(policy, metadata, list[i], depth + 1,
                                  address + QList<qsizetype>{i}, out);
        return list;
    }
    out.error = "list-policy-value";
    return {};
}
ConfigValueResult transformList(const ConfigRecordListPolicySpec &policy,
                                const QList<ConfigValueSelector> &address, const QVariant &value,
                                const std::function<ConfigValueResult(QVariantList)> &transform,
                                qsizetype depth = 0, std::optional<ConfigDescriptor> node = {},
                                bool ensure = false, bool absentOnly = false) {
    const auto fail = [] {
        return ConfigValueResult{{}, {ConfigDiagnosticBuilder().make({}, "list-edit-operation")}};
    };
    if (depth < address.size()) {
        if (depth >= policy.listPath.size())
            return fail();
        const auto selected = address[depth];
        const auto expected = policy.listPath[depth];
        if (selected.kind == Selector::Kind::Each ||
            (selected.kind == Selector::Kind::Match && expected.kind != Selector::Kind::Each) ||
            (expected.kind != Selector::Kind::Each && !(selected == expected)))
            return fail();
        if (selected.kind == Selector::Kind::Key && mapValue(value)) {
            auto map = value.toMap();
            std::optional<ConfigDescriptor> child;
            if (node && node->type == ConfigDescriptor::Type::Record &&
                node->fields.contains(selected.key))
                child = node->fields.value(selected.key);
            else if (node && node->type == ConfigDescriptor::Type::Map)
                child = node->element.first();
            QVariant next = map.value(selected.key);
            if (!map.contains(selected.key)) {
                // copy creates its final list even without ensure; ancestors need ensure.
                if (absentOnly && depth + 1 == address.size()) {
                    if (!child || child->type != ConfigDescriptor::Type::List)
                        return fail();
                } else {
                    if (!ensure || !child)
                        return fail();
                    if (child->type == ConfigDescriptor::Type::Record ||
                        child->type == ConfigDescriptor::Type::Map)
                        next = QVariantMap{};
                    else if (child->type == ConfigDescriptor::Type::List)
                        next = QVariantList{};
                    else
                        return fail();
                }
            }
            auto result = transformList(policy, address, next, transform, depth + 1, child, ensure,
                                        absentOnly);
            if (!result.ok())
                return result;
            map[selected.key] = result.value;
            return {map, {}};
        }
        if ((selected.kind == Selector::Kind::Index || selected.kind == Selector::Kind::Match) &&
            listValue(value)) {
            auto list = value.toList();
            auto index = selected.index;
            const auto child = node && node->type == ConfigDescriptor::Type::List
                                   ? std::optional<ConfigDescriptor>(node->element.first())
                                   : std::nullopt;
            if (selected.kind == Selector::Kind::Match) {
                if (!child || child->type != ConfigDescriptor::Type::Record)
                    return fail();
                const auto field = child->fields.constFind(selected.key);
                if (field == child->fields.cend() ||
                    field->type != ConfigDescriptor::Type::String || field->optional ||
                    field->nullable)
                    return fail();
                index = -1;
                for (qsizetype i = 0; i < list.size(); ++i) {
                    if (!mapValue(list[i]))
                        return fail();
                    const auto candidate = list[i].toMap().value(selected.key);
                    if (index < 0 && candidate.metaType().id() == QMetaType::QString &&
                        candidate.toString() == selected.value)
                        index = i;
                }
                if (index < 0 && ensure) {
                    index = list.size();
                    list.append(QVariantMap{{selected.key, selected.value}});
                }
            }
            if (index < 0 || index >= list.size())
                return fail();
            auto result = transformList(policy, address, list[index], transform, depth + 1, child,
                                        ensure, absentOnly);
            if (!result.ok())
                return result;
            list[index] = result.value;
            return {list, {}};
        }
        return fail();
    }
    if (depth != policy.listPath.size())
        return fail();
    if (absentOnly)
        return value.isValid() ? fail() : transform({});
    if (!listValue(value))
        return fail();
    return transform(value.toList());
}
ConfigValueResult edit(const ConfigRecordListPolicySpec &policy,
                       const ConfigRecordListMetadata &metadata, const QVariant &value,
                       const ConfigRecordListEditOptions &options, const ConfigSchema *schema) {
    return transformList(
        policy, options.listAddress, value,
        [&](QVariantList raw) -> ConfigValueResult {
            const auto fail = [] {
                return ConfigValueResult{
                    {}, {ConfigDiagnosticBuilder().make({}, "list-edit-operation")}};
            };
            for (const auto &operation : options.edits) {
                QList<qsizetype> mapping;
                QString error;
                const auto display = normalize(policy, metadata, raw, mapping, error);
                if (!error.isEmpty() || operation.index < 0 || operation.index > display.size())
                    return fail();
                if (operation.op == ConfigRecordListEdit::Op::Insert) {
                    if (!mapValue(operation.value))
                        return fail();
                    const auto index = operation.index < mapping.size() ? mapping[operation.index]
                                       : mapping.isEmpty()              ? raw.size()
                                                                        : mapping.last() + 1;
                    raw.insert(index, operation.value);
                    continue;
                }
                if (operation.index == mapping.size())
                    return fail();
                const auto index = mapping[operation.index];
                switch (operation.op) {
                case ConfigRecordListEdit::Op::Set:
                case ConfigRecordListEdit::Op::Unset: {
                    if (operation.field.isEmpty())
                        return fail();
                    auto row = raw[index].toMap();
                    if (operation.op == ConfigRecordListEdit::Op::Set)
                        row[operation.field] = operation.value;
                    else
                        row.remove(operation.field);
                    raw[index] = row;
                    break;
                }
                case ConfigRecordListEdit::Op::Remove:
                    raw.removeAt(index);
                    break;
                case ConfigRecordListEdit::Op::Move: {
                    if (operation.to < 0 || operation.to >= mapping.size())
                        return fail();
                    if (operation.to == operation.index)
                        break;
                    const auto row = raw.takeAt(index);
                    QList<qsizetype> remaining;
                    normalize(policy, metadata, raw, remaining, error);
                    const auto target = operation.to < remaining.size() ? remaining[operation.to]
                                        : remaining.isEmpty()           ? raw.size()
                                                                        : remaining.last() + 1;
                    raw.insert(target, row);
                    break;
                }
                case ConfigRecordListEdit::Op::Insert:
                    break;
                default:
                    return fail();
                }
            }
            return {raw, {}};
        },
        0, schema ? schema->descriptor(policy.path) : std::nullopt, options.ensure);
}
} // namespace
QString validateRecordListPolicy(const ConfigSchema &schema,
                                 const ConfigRecordListPolicySpec &policy) {
    auto node = schema.descriptor(policy.path);
    if (!node || node->type == ConfigDescriptor::Type::Group || policy.policyId.isEmpty() ||
        policy.keyField.isEmpty())
        return "list-policy-declaration";
    for (const auto &selector : policy.listPath) {
        if (selector.kind == Selector::Kind::Key && (node->type == ConfigDescriptor::Type::Record ||
                                                     node->type == ConfigDescriptor::Type::Map)) {
            if (node->type == ConfigDescriptor::Type::Map)
                node = node->element.first();
            else if (node->fields.contains(selector.key))
                node = node->fields.value(selector.key);
            else
                return "list-policy-declaration";
        } else if ((selector.kind == Selector::Kind::Index ||
                    selector.kind == Selector::Kind::Each) &&
                   node->type == ConfigDescriptor::Type::List && selector.index >= 0)
            node = node->element.first();
        else
            return "list-policy-declaration";
    }
    if (node->type != ConfigDescriptor::Type::List || node->element.size() != 1 ||
        node->element.first().type != ConfigDescriptor::Type::Record)
        return "list-policy-declaration";
    const auto key = node->element.first().fields.constFind(policy.keyField);
    if (key == node->element.first().fields.cend() || key->type != ConfigDescriptor::Type::String ||
        key->optional || key->nullable)
        return "list-policy-declaration";
    return {};
}
ConfigRecordListProjection projectRecordLists(const ConfigRecordListPolicySpec &policy,
                                              const ConfigRecordListMetadata &metadata,
                                              const QVariant &value) {
    ConfigRecordListProjection result;
    result.value = project(policy, metadata, value, 0, {}, result);
    return result;
}
ConfigValueResult editRecordListValue(const ConfigRecordListPolicySpec &policy,
                                      const ConfigRecordListMetadata &metadata,
                                      const QVariant &value,
                                      const ConfigRecordListEditOptions &options,
                                      const ConfigSchema *schema) {
    if (options.ensure && options.edits.isEmpty())
        return {{}, {ConfigDiagnosticBuilder().make({}, "list-edit-operation")}};
    return edit(policy, metadata, value, options, schema);
}
QString transferRecordListValue(const ConfigRecordListPolicySpec &fromPolicy,
                                const ConfigRecordListMetadata &fromMetadata, QVariant &fromValue,
                                const ConfigRecordListPolicySpec &toPolicy,
                                const ConfigRecordListMetadata &toMetadata, QVariant &toValue,
                                const ConfigRecordListEdit &operation, const ConfigSchema *schema) {
    const auto fail = [] {
        return ConfigValueResult{{}, {ConfigDiagnosticBuilder().make({}, "list-edit-operation")}};
    };
    QVariant row;
    if (operation.from.ensure)
        return "list-edit-operation";
    const auto removed = transformList(
        fromPolicy, operation.from.listAddress, fromValue,
        [&](QVariantList raw) -> ConfigValueResult {
            QList<qsizetype> mapping;
            QString error;
            normalize(fromPolicy, fromMetadata, raw, mapping, error);
            if (!error.isEmpty() || operation.from.index < 0 ||
                operation.from.index >= mapping.size())
                return fail();
            row = raw.takeAt(mapping[operation.from.index]);
            return {raw, {}};
        },
        0, schema ? schema->descriptor(fromPolicy.path) : std::nullopt);
    if (!removed.ok())
        return "list-edit-operation";
    const bool sameLeaf = &fromValue == &toValue;
    const auto inserted = transformList(
        toPolicy, operation.destination.listAddress, sameLeaf ? removed.value : toValue,
        [&](QVariantList raw) -> ConfigValueResult {
            QList<qsizetype> mapping;
            QString error;
            normalize(toPolicy, toMetadata, raw, mapping, error);
            const auto index = operation.destination.index;
            if (!error.isEmpty() || index < 0 || index > mapping.size())
                return fail();
            const auto target = index < mapping.size() ? mapping[index]
                                : mapping.isEmpty()    ? raw.size()
                                                       : mapping.last() + 1;
            raw.insert(target, row);
            return {raw, {}};
        },
        0, schema ? schema->descriptor(toPolicy.path) : std::nullopt, operation.destination.ensure);
    if (!inserted.ok())
        return "list-edit-operation";
    fromValue = sameLeaf ? inserted.value : removed.value;
    if (!sameLeaf)
        toValue = inserted.value;
    return {};
}
QString copyRecordListValue(const ConfigRecordListPolicySpec &fromPolicy,
                            const ConfigRecordListMetadata &fromMetadata, const QVariant &fromValue,
                            const ConfigRecordListPolicySpec &toPolicy,
                            const ConfigRecordListMetadata &toMetadata, QVariant &toValue,
                            const ConfigRecordListEdit &operation, const ConfigSchema &schema) {
    if (operation.from.ensure || (!mapValue(toValue) && !listValue(toValue)))
        return "list-edit-operation";
    QVariantList source;
    const auto read = transformList(
        fromPolicy, operation.from.listAddress, fromValue,
        [&](QVariantList raw) -> ConfigValueResult {
            source = raw;
            return {raw, {}};
        },
        0, schema.descriptor(fromPolicy.path));
    if (!read.ok())
        return "list-edit-operation";
    QList<qsizetype> a, b;
    QString error;
    const auto from = normalize(fromPolicy, fromMetadata, source, a, error);
    const auto to = normalize(toPolicy, toMetadata, source, b, error);
    if (!error.isEmpty())
        return "list-edit-operation";
    if (from != to)
        return "list-edit-conflict";
    const auto copied = transformList(
        toPolicy, operation.destination.listAddress, toValue,
        [&](QVariantList) -> ConfigValueResult { return {source, {}}; }, 0,
        schema.descriptor(toPolicy.path), operation.destination.ensure, true);
    if (!copied.ok())
        return "list-edit-operation";
    toValue = copied.value;
    return {};
}
} // namespace configqml
