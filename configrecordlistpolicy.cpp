// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configrecordlistpolicy.h"
#include <QSet>

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
ConfigValueResult edit(const ConfigRecordListPolicySpec &policy,
                       const ConfigRecordListMetadata &metadata, const QVariant &value,
                       const ConfigRecordListEditOptions &options, qsizetype depth) {
    const auto fail = [] {
        return ConfigValueResult{{}, {ConfigDiagnosticBuilder().make({}, "list-edit-operation")}};
    };
    if (depth < options.listAddress.size()) {
        if (depth >= policy.listPath.size())
            return fail();
        const auto selected = options.listAddress[depth];
        const auto expected = policy.listPath[depth];
        if (selected.kind == Selector::Kind::Each ||
            (expected.kind != Selector::Kind::Each && !(selected == expected)))
            return fail();
        if (selected.kind == Selector::Kind::Key && mapValue(value)) {
            auto map = value.toMap();
            if (!map.contains(selected.key))
                return fail();
            auto result = edit(policy, metadata, map.value(selected.key), options, depth + 1);
            if (!result.ok())
                return result;
            map[selected.key] = result.value;
            return {map, {}};
        }
        if (selected.kind == Selector::Kind::Index && listValue(value)) {
            auto list = value.toList();
            if (selected.index < 0 || selected.index >= list.size())
                return fail();
            auto result = edit(policy, metadata, list[selected.index], options, depth + 1);
            if (!result.ok())
                return result;
            list[selected.index] = result.value;
            return {list, {}};
        }
        return fail();
    }
    if (depth != policy.listPath.size() || !listValue(value))
        return fail();
    auto raw = value.toList();
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
                                      const ConfigRecordListEditOptions &options) {
    return edit(policy, metadata, value, options, 0);
}
} // namespace configqml
