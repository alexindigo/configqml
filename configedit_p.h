// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configselection_p.h"
#include "configresult.h"

namespace configqml {
struct ConfigEdit {
    enum class Kind { Set, RemoveOverride };
    Kind kind = Kind::RemoveOverride;
    QVariant value;
};
// An omitted entry is no edit. Null is an admitted Set, never removal.
struct ConfigEditBatch {
    ConfigSelection selection;
    QMap<ConfigPath, ConfigEdit> operations;
    QMap<ConfigPath, ConfigPathState> fences;
    quint64 sequence = 0;
    QString error;
    static ConfigEditBatch capture(const ConfigSelection &selection,
                                   const QMap<ConfigPath, std::optional<QVariant>> &edits);
    QMap<ConfigPath, std::optional<QVariant>> layerEdits() const;
};
} // namespace configqml
