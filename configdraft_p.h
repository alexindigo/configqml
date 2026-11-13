// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include <atomic>
#include <memory>

namespace configqml {
// Immutable checked draft edits, never a copied store/raw tree or persistence layer.
struct ConfigDraftPreview {
    QString id;
    std::shared_ptr<std::atomic_bool> lifetime;
    QList<ConfigPath> paths;
    QMap<ConfigPath, std::optional<QVariant>> edits;
};
ConfigSchema draftPreviewSchema(const ConfigSchema &schema, const QList<ConfigPath> &paths);
} // namespace configqml
