// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configbackend.h"
#include "configlayers.h"

namespace configqml {
struct ConfigPathState {
    ConfigInfo provenance;
    quint64 latestInput = 0;
    quint64 workerCovered = 0;
    quint64 pathGeneration = 0;
    quint64 publicationGeneration = 0;
    quint64 contentRevision = 0;
    quint64 intentRevision = 0; // Worker-accepted intent, independent of GUI ingress generations.
};
struct ConfigInput {
    enum class Kind { User, Reset, Runtime, ClearRuntime, Initialize, Group };
    Kind kind = Kind::User;
    quint64 sequence = 0;
    ConfigPath path;
    QVariant value;
    QString target;
    QMap<ConfigPath, quint64> pathGenerations;
    QMap<ConfigPath, std::optional<QVariant>> edits;
    QMap<ConfigPath, ConfigPathState> fences;
};
struct ConfigPublication {
    QString sourceId;
    bool success = false;
    ConfigRevision revision;
    QString state = "known";
    ConfigError error;
};
struct ConfigResult {
    quint64 requestId = 0;
    QString storeId;
    QString operation;
    QString status; // Save/flush/close: saved | nothing_to_save | failed.
    quint64 inputPrefix = 0;
    QMap<QString, ConfigRevision> coveredRevisions;
    QList<ConfigPublication> publications;
    QString publicationState = "known";
    ConfigError error;
    QString maskedBy;
    QMap<ConfigPath, QString> masks; // Grouped paths, never setting values.
    QString description() const {
        return maskedBy.isEmpty() ? QString() : QStringLiteral("masked by ") + maskedBy;
    }
};
struct ConfigRequestHandle {
    quint64 requestId = 0;
    quint64 inputSequence = 0;
    bool accepted = false; // Synchronous ingress only, not worker admission.
    QList<ConfigDiagnostic> errors;
};
struct ConfigSnapshot {
    QMap<ConfigPath, QVariant> values;
    QMap<ConfigPath, ConfigPathState> information;
    quint64 generation = 0;
    quint64 topology = 1; // Immutable schema/source topology in M2.
    quint64 inputPrefix = 0;
};
} // namespace configqml
Q_DECLARE_METATYPE(configqml::ConfigResult)
Q_DECLARE_METATYPE(configqml::ConfigInput)
