// SPDX-License-Identifier: LGPL-3.0-or-later
#include "confighandle_p.h"

namespace configqml {
ConfigHandleSelection ConfigHandleSelection::capture(const ConfigSelection &selection,
                                                     const ConfigPath &prefix) {
    ConfigHandleSelection result;
    result.m_selection = selection;
    result.m_prefix = ownPath(prefix);
    if (!selection.matchesSchema(selection.schema))
        result.m_selection.error = "handle-schema";
    if (!selection.schema.descriptor(prefix))
        result.m_selection.error = "handle-selection";
    for (const auto &path : selection.leaves)
        if (!startsWith(path, prefix))
            result.m_selection.error = "handle-selection";
    // Validate the allowed leaf boundary too, rather than trusting a fabricated
    // private selection to introduce an atomic record member as a new leaf.
    if (result.valid()) {
        const auto checked =
            ConfigSelection::capture(selection.schema, selection.leaves, selection.schema.leaves(),
                                     selection.target, "handle-selection");
        if (!checked.valid() || checked.leaves != selection.leaves)
            result.m_selection.error = "handle-selection";
    }
    return result;
}
ConfigHandleSelection ConfigHandleSelection::restrict(const QList<ConfigPath> &absolute,
                                                      const ConfigPath &prefix) const {
    if (!valid())
        return *this;
    auto selected = ConfigSelection::capture(m_selection.schema, absolute, m_selection.leaves,
                                             m_selection.target, "handle-selection");
    selected.owner = m_selection.owner;
    selected.lifetime = m_selection.lifetime;
    return capture(selected, prefix);
}
ConfigHandleSelection ConfigHandleSelection::scope(const ConfigPath &relative) const {
    const auto absolute = m_prefix + ownPath(relative);
    return restrict({absolute}, absolute);
}
ConfigHandleSelection ConfigHandleSelection::select(const QList<ConfigPath> &relative) const {
    QList<ConfigPath> absolute;
    for (const auto &path : relative)
        absolute.append(m_prefix + ownPath(path));
    return restrict(absolute, m_prefix);
}
QStringList ConfigHandleSelection::children(bool groups) const {
    QStringList result;
    const auto node = descriptor();
    if (!node || node->type != ConfigDescriptor::Type::Group)
        return result;
    for (auto it = node->fields.cbegin(); it != node->fields.cend(); ++it)
        if ((it->type == ConfigDescriptor::Type::Group) == groups)
            result.append(it.key());
    return result;
}
QStringList ConfigHandleSelection::scopes() const { return children(true); }
QStringList ConfigHandleSelection::fields() const { return children(false); }
std::optional<ConfigDescriptor> ConfigHandleSelection::descriptor() const {
    if (!valid())
        return std::nullopt;
    // Schema metadata is selected structurally; no raw/effective values or type
    // inference enter lookup. Records, maps and lists remain atomic descriptors.
    auto descriptor = m_selection.schema.descriptor(m_prefix);
    if (!descriptor || descriptor->type != ConfigDescriptor::Type::Group)
        return descriptor;
    descriptor->fields.clear();
    const auto declared = m_selection.schema.descriptor(m_prefix);
    for (auto it = declared->fields.cbegin(); it != declared->fields.cend(); ++it) {
        const auto child = scope({it.key()});
        if (child.valid())
            descriptor->fields.insert(it.key(), *child.descriptor());
    }
    return descriptor;
}
} // namespace configqml
