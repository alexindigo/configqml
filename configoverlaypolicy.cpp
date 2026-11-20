// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configoverlay_p.h"
#include "configjsoncodec.h"
#include <QMutexLocker>

namespace configqml {
quint64 ConfigOverlayEndpoint::revision(const ConfigPath &path) const {
    QMutexLocker lock(&mutex);
    return ingress.value(path);
}
void ConfigOverlayEndpoint::advance(const QList<ConfigPath> &paths) {
    QMutexLocker lock(&mutex);
    for (const auto &path : paths)
        ++ingress[path];
}
QVariant overlayMerge(const QVariant &base, const QVariant &overrides) {
    if (base.metaType().id() != QMetaType::QVariantMap ||
        overrides.metaType().id() != QMetaType::QVariantMap)
        return overrides;
    auto result = base.toMap();
    const auto upper = overrides.toMap();
    for (auto it = upper.cbegin(); it != upper.cend(); ++it)
        result[it.key()] = result.contains(it.key())
                               ? overlayMerge(result.value(it.key()), it.value())
                               : it.value();
    return result;
}
template <typename Normalize>
static bool mappingMoved(const QVariantList &before, const QVariantList &after,
                         const QVariant &previous, const QVariant &selected, Normalize normalize) {
    if (previous == selected)
        return false;
    const auto oldRow = encodeJsonValue(previous), newRow = encodeJsonValue(selected);
    if (!oldRow.ok() || !newRow.ok() || oldRow.bytes == newRow.bytes)
        return false;
    const auto count = [&](const QVariantList &rows, qsizetype &oldCount, qsizetype &newCount) {
        for (const auto &row : rows) {
            const auto encoded = encodeJsonValue(normalize(row));
            if (!encoded.ok())
                return false;
            oldCount += encoded.bytes == oldRow.bytes;
            newCount += encoded.bytes == newRow.bytes;
        }
        return true;
    };
    qsizetype oldBefore = 0, oldAfter = 0, newBefore = 0, newAfter = 0;
    if (!count(before, oldBefore, newBefore) || !count(after, oldAfter, newAfter))
        return false;
    // Preserve multiplicities of the relocated witnesses, not the entire owner:
    // unrelated rows can change too. Merely creating a duplicate is not a move.
    const bool movedElsewhere = oldBefore > 0 && oldBefore == oldAfter;
    const bool cameFromElsewhere = newBefore > 0 && newBefore == newAfter;
    return before.size() == after.size() ? movedElsewhere && cameFromElsewhere
                                         : movedElsewhere || cameFromElsewhere;
}
bool overlayPlacementMoved(const QVariant &before, const QVariant &after,
                           const QList<ConfigValueSelector> &address, const QString &overrideKey) {
    if (before == after)
        return false;
    auto withoutOverride = [&](const QVariant &value, const QList<ConfigValueSelector> &tail) {
        const auto record = overlayAt(value, tail);
        if (!record || record->metaType().id() != QMetaType::QVariantMap)
            return value;
        auto map = record->toMap();
        map.remove(overrideKey);
        return overlayReplace(value, tail, map).value_or(value);
    };
    QVariant old = before, current = after;
    for (qsizetype depth = 0; depth < address.size(); ++depth) {
        const auto selector = address[depth];
        if (selector.kind == ConfigValueSelector::Kind::Index &&
            old.metaType().id() == QMetaType::QVariantList &&
            current.metaType().id() == QMetaType::QVariantList) {
            const auto a = old.toList(), b = current.toList();
            const auto index = selector.index;
            if (index >= a.size() || index >= b.size())
                return (index < a.size()) != (index < b.size());
            if (mappingMoved(a, b, a[index], b[index], [](const QVariant &row) { return row; }))
                return true;
            const auto tail = address.mid(depth + 1);
            const auto previous = withoutOverride(a[index], tail);
            const auto selected = withoutOverride(b[index], tail);
            if (mappingMoved(a, b, previous, selected,
                             [&](const QVariant &row) { return withoutOverride(row, tail); }))
                return true;
        }
        if (selector.kind == ConfigValueSelector::Kind::Key &&
            old.metaType().id() == QMetaType::QVariantMap &&
            current.metaType().id() == QMetaType::QVariantMap) {
            const auto a = old.toMap(), b = current.toMap();
            if (a.contains(selector.key) && b.contains(selector.key)) {
                if (mappingMoved(a.values(), b.values(), a.value(selector.key),
                                 b.value(selector.key), [](const QVariant &row) { return row; }))
                    return true;
                const auto tail = address.mid(depth + 1);
                const auto previous = withoutOverride(a.value(selector.key), tail);
                const auto selected = withoutOverride(b.value(selector.key), tail);
                if (mappingMoved(a.values(), b.values(), previous, selected,
                                 [&](const QVariant &row) { return withoutOverride(row, tail); }))
                    return true;
            }
        }
        const auto a = overlayAt(old, {selector}), b = overlayAt(current, {selector});
        if (!a || !b)
            return bool(a) != bool(b);
        old = *a;
        current = *b;
    }
    return false;
}
static bool schemaEqual(const ConfigDescriptor &descriptor, const QVariant &a, const QVariant &b) {
    const auto left = descriptor.validate(a), right = descriptor.validate(b);
    return left.ok() && right.ok() && left.value == right.value;
}
QVariantMap overlayDifference(const ConfigSchema &schema, const QVariantMap &base,
                              const QVariantMap &edited, const QVariantMap &original) {
    // Retain the private complete-authored-value symbol for binary compatibility.
    // Overlay commands use the explicit-edit overload, never their presentation.
    QMap<ConfigPath, std::optional<QVariant>> edits;
    for (const auto &path : schema.leaves())
        edits[path] = valueAt(edited, path);
    return overlayDifference(schema, base, edits, original);
}
QVariantMap overlayDifference(const ConfigSchema &schema, const QVariantMap &base,
                              const QMap<ConfigPath, std::optional<QVariant>> &edits,
                              const QVariantMap &original) {
    auto result = original;
    for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
        const auto descriptor = schema.descriptor(it.key());
        if (!descriptor || descriptor->type == ConfigDescriptor::Type::Group)
            continue;
        const auto inherited = valueAt(base, it.key());
        if (!it.value() || (inherited && schemaEqual(*descriptor, *it.value(), *inherited))) {
            if (valueAt(result, it.key()))
                removeValue(result, it.key());
        } else
            putValue(result, it.key(), *it.value());
    }
    return result;
}
std::optional<QVariant> overlayAt(const QVariant &root, const QList<ConfigValueSelector> &path) {
    QVariant value = root;
    for (const auto &selector : path) {
        if (selector.kind == ConfigValueSelector::Kind::Key &&
            value.metaType().id() == QMetaType::QVariantMap) {
            const auto map = value.toMap();
            if (!map.contains(selector.key))
                return {};
            value = map.value(selector.key);
        } else if (selector.kind == ConfigValueSelector::Kind::Index &&
                   value.metaType().id() == QMetaType::QVariantList && selector.index >= 0 &&
                   selector.index < value.toList().size())
            value = value.toList()[selector.index];
        else
            return {};
    }
    return value;
}
std::optional<QVariant> overlayReplace(const QVariant &root, const QList<ConfigValueSelector> &path,
                                       const QVariant &replacement) {
    if (path.isEmpty())
        return replacement;
    const auto child = overlayAt(root, {path.first()});
    if (!child)
        return {};
    const auto edited = overlayReplace(*child, path.mid(1), replacement);
    if (!edited)
        return {};
    if (path.first().kind == ConfigValueSelector::Kind::Key) {
        auto map = root.toMap();
        map[path.first().key] = *edited;
        return map;
    }
    auto list = root.toList();
    list[path.first().index] = *edited;
    return list;
}
std::optional<ConfigDescriptor> overlayDescriptor(ConfigDescriptor descriptor,
                                                  const QList<ConfigValueSelector> &path) {
    for (const auto &selector : path) {
        if (selector.kind == ConfigValueSelector::Kind::Key &&
            descriptor.type == ConfigDescriptor::Type::Record &&
            descriptor.fields.contains(selector.key))
            descriptor = descriptor.fields.value(selector.key);
        else if ((selector.kind == ConfigValueSelector::Kind::Key &&
                  descriptor.type == ConfigDescriptor::Type::Map) ||
                 (selector.kind == ConfigValueSelector::Kind::Index && selector.index >= 0 &&
                  descriptor.type == ConfigDescriptor::Type::List))
            descriptor = descriptor.element.first();
        else
            return {};
    }
    return descriptor;
}
QList<ConfigValueSelector>
overlayRawAddress(const QVariant &raw, const QList<ConfigValueSelector> &display,
                  const QList<ConfigRecordListPolicySpec> &policies,
                  const QMap<QString, ConfigRecordListMetadata> &metadata, QString &error) {
    auto address = display;
    // Each policy's row map is relative to its input. Reverse composition maps
    // the final display address back through every disjoint/nested projection.
    QList<QPair<ConfigRecordListPolicySpec, ConfigRecordListProjection>> projections;
    auto value = raw;
    for (const auto &policy : policies) {
        auto projected = projectRecordLists(policy, metadata.value(policy.policyId), value);
        if (!projected.error.isEmpty()) {
            error = "overlay-conflict";
            return {};
        }
        value = projected.value;
        projections.append({policy, projected});
    }
    for (auto it = projections.crbegin(); it != projections.crend(); ++it) {
        const auto &policy = it->first;
        if (address.size() <= policy.listPath.size())
            continue;
        bool matches = true;
        QList<qsizetype> indices;
        for (qsizetype i = 0; i < policy.listPath.size(); ++i) {
            const auto expected = policy.listPath[i], actual = address[i];
            matches &= expected.kind == ConfigValueSelector::Kind::Each
                           ? actual.kind == ConfigValueSelector::Kind::Index
                           : expected == actual;
            if (actual.kind == ConfigValueSelector::Kind::Index)
                indices.append(actual.index);
        }
        if (!matches)
            continue;
        auto &row = address[policy.listPath.size()];
        const auto mapping = it->second.rows.value(indices);
        if (row.kind != ConfigValueSelector::Kind::Index || row.index < 0 ||
            row.index >= mapping.size()) {
            error = "overlay-conflict";
            return {};
        }
        row.index = mapping[row.index];
    }
    return address;
}
} // namespace configqml
