// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configresult.h"
#include <atomic>
#include <QSet>

namespace configqml {
// Accepted metadata is written by the worker before ordered completion, then
// immutable. Only invalidation is shared after that handoff; raw recovery stays
// on the worker. The attempt has one writer until completion, like overlay IO.
struct ConfigApplyWitness {
    QMap<ConfigPath, ConfigPathState> fences;
    std::atomic_bool valid{true};
};
struct ConfigApplyAttempt {
    QString ownerId;
    bool publicationFence = false;
    std::shared_ptr<ConfigApplyWitness> expected, admitted;
};
struct ConfigApplyCapture {
    std::weak_ptr<ConfigApplyWitness> witness;
    QMap<ConfigPath, ConfigPathState> fences;
    QMap<ConfigPath, QVariantList> sourceFacts;
    QMap<ConfigPath, std::optional<QVariant>> edits;
    QSet<ConfigPath> ownConfirmation;
    QSet<ConfigPath> rejectedIngress;
    bool publicationFence = false;
};
} // namespace configqml
