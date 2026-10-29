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
struct ConfigEvidenceReference {
    QString evidenceId, directory, sourceId, phase, role;
    quint64 observationOrder = 0;
};
struct ConfigSourceState {
    QString sourceId;
    ConfigPath prefix;
    QString path, presence = "unknown", load = "not_read";
    ConfigRevision revision;
    bool quarantined = false;
    QString fallbackFor, fallbackState = "ordinary";
    ConfigError diagnostic;
    QList<ConfigEvidenceReference> evidence;
};
struct ConfigSourceFact {
    ConfigPath path;
    bool present = false;
    QVariant value;
    QString sourceId;
    ConfigRevision revision;
};
struct ConfigSourceAccepted {
    QString eventId;
    quint64 publicationGeneration = 0, inputPrefix = 0;
    QList<QPair<QString, ConfigRevision>> sources;
    QList<ConfigPath> changedPaths;
    QString origin, cause;
    QList<ConfigSourceFact> facts;
};
struct ConfigCoverage {
    ConfigPath path;
    QString sourceId;
    quint64 inputThrough = 0, intentRevision = 0;
};
struct ConfigGroupEdit {
    ConfigPath path;
    bool present = true;
    QVariant value;
};
struct ConfigRetainedGroupOptions {
    QString requestKey;
    QList<ConfigPath> paths;
    QList<ConfigGroupEdit> edits;
    QStringList dependencies;
};
struct ConfigGroupAttempt {
    QString attemptId;
    quint64 attemptNumber = 0, requestId = 0;
    QString status, publicationState = "known";
    ConfigError error;
};
struct ConfigGroupInfo {
    QString groupToken, requestKey, state;
    quint64 capturePrefix = 0;
    QList<ConfigCoverage> coverage;
    QStringList dependencies;
    QList<ConfigGroupAttempt> attempts;
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
    // Only an unaccepted group can release its own covered ingress fence.
    QMap<ConfigPath, quint64> rejectedPathGenerations;
    QList<ConfigEvidenceReference> evidence;
    QString policyId;
    quint64 metadataRevision = 0;
    QList<ConfigCoverage> coverage;
    QString groupToken, requestKey, attemptId;
    quint64 capturePrefix = 0, attemptNumber = 0;
    QStringList dependencies, unresolvedGroups, excludedGroups;
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
