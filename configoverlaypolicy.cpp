// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configoverlay_p.h"
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
static bool schemaEqual(const ConfigDescriptor &descriptor, const QVariant &a, const QVariant &b) {
    const auto left = descriptor.validate(a), right = descriptor.validate(b);
    return left.ok() && right.ok() && left.value == right.value;
}
QVariantMap overlayDifference(const ConfigSchema &schema, const QVariantMap &base,
                              const QVariantMap &edited, const QVariantMap &original) {
    QVariantMap result;
    const auto fields = schema.descriptor({})->fields;
    for (auto it = original.cbegin(); it != original.cend(); ++it)
        if (!fields.contains(it.key()))
            result[it.key()] = it.value();
    for (auto it = edited.cbegin(); it != edited.cend(); ++it) {
        const auto descriptor = fields.constFind(it.key());
        if (descriptor == fields.cend())
            continue;
        if (!base.contains(it.key()) ||
            !schemaEqual(descriptor.value(), it.value(), base.value(it.key())))
            result[it.key()] = it.value();
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
