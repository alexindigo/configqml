// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include <atomic>

namespace configqml {
struct ConfigOverlayEndpoint;
// Selected declared leaves are the unit of admission and observation. Raw record
// addresses remain target codecs, never additional schema/persistence paths.
struct ConfigSelection {
    enum class Target { UserLayer, RuntimeLayer, RecordSlot, ReadOnly };
    ConfigSchema schema{ConfigDescriptor::group({})};
    std::shared_ptr<const char> schemaIdentity;
    std::shared_ptr<ConfigOverlayEndpoint> owner;
    std::shared_ptr<std::atomic_bool> lifetime;
    QList<ConfigPath> leaves;
    Target target = Target::ReadOnly;
    QString error;
    bool valid() const { return error.isEmpty(); }
    bool matchesSchema(const ConfigSchema &other) const;
    bool contains(const ConfigPath &path) const { return leaves.contains(path); }
    static ConfigSelection capture(const ConfigSchema &schema, const QList<ConfigPath> &paths,
                                   const QList<ConfigPath> &allowed, Target target,
                                   const QString &error, bool emptyMeansAll = false,
                                   bool rejectDuplicates = true);
};
} // namespace configqml
