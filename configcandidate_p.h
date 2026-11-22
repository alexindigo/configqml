// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configedit_p.h"
#include "configrecordlistpolicy.h"
#include <functional>

namespace configqml {
struct ConfigCandidateFact {
    QVariant effective;
    bool targetPresent = false;
    ConfigEdit::Kind pending = ConfigEdit::Kind::RemoveOverride;
    bool edited = false;
    QVariant authored;
    QString maskedBy;
    bool operator==(const ConfigCandidateFact &other) const {
        return effective == other.effective && targetPresent == other.targetPresent &&
               pending == other.pending && edited == other.edited && authored == other.authored &&
               maskedBy == other.maskedBy;
    }
};
struct ConfigCandidate {
    std::unique_ptr<ConfigLayers> layers;
    QMap<ConfigPath, ConfigCandidateFact> facts;
    QString error;
    bool valid() const { return error.isEmpty() && layers != nullptr; }
};
// The same record-list transformation is used by candidate evaluation and the
// publication cache. It produces values only; no application effects/intent.
ConfigValueResult projectCandidateLeaf(const ConfigPath &path, const QVariant &unmasked,
                                       const QMap<QString, ConfigRecordListPolicySpec> &policies,
                                       const QMap<QString, ConfigRecordListMetadata> &metadata);
ConfigCandidate resolveCandidate(const ConfigLayers &base, const ConfigEditBatch &batch,
                                 const QMap<QString, ConfigRecordListPolicySpec> &policies,
                                 const QMap<QString, ConfigRecordListMetadata> &metadata,
                                 const std::function<QString(const ConfigInput &)> &admission = {});
QMap<ConfigPath, QVariant> candidatePresentation(const ConfigCandidate &candidate);
} // namespace configqml
