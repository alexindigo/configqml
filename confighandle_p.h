// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configselection_p.h"

namespace configqml {
// Immutable schema identity beneath generated handles. This describes a selected
// namespace only; P1/P2/P3 retain all value, candidate and observation ownership.
class ConfigHandleSelection {
public:
    static ConfigHandleSelection capture(const ConfigSelection &selection,
                                         const ConfigPath &prefix = {});
    bool valid() const { return m_selection.valid(); }
    QString error() const { return m_selection.error; }
    ConfigPath prefix() const { return m_prefix; }
    ConfigSelection selection() const { return m_selection; }
    ConfigHandleSelection scope(const ConfigPath &relative) const;
    ConfigHandleSelection select(const QList<ConfigPath> &relative) const;
    QStringList scopes() const;
    QStringList fields() const;
    std::optional<ConfigDescriptor> descriptor() const;

private:
    ConfigHandleSelection() { m_selection.error = "handle-selection"; }
    ConfigHandleSelection restrict(const QList<ConfigPath> &absolute,
                                   const ConfigPath &prefix) const;
    QStringList children(bool groups) const;
    ConfigSelection m_selection;
    ConfigPath m_prefix;
};
} // namespace configqml
