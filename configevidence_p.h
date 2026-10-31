// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configresult.h"
namespace configqml {
struct ConfigEvidenceObservation {
    QString storeId, requestId, groupToken, attemptId;
    QString sourceId, phase, presence, bytesState, role;
    ConfigRevision revision;
    QByteArray bytes;
    quint64 observationOrder = 0;
};
std::optional<ConfigEvidenceReference> retainEvidence(const QString &root,
                                                      const ConfigEvidenceObservation &observation);
} // namespace configqml
