// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configobservation.h"
#include "configcandidate_p.h"

namespace configqml {
class ConfigReadProjection;
struct ConfigWorkerPacket;
struct ConfigObservationDependency {
    std::weak_ptr<ConfigOverlayEndpoint> owner;
    quint64 incarnation = 0, delivery = 0;
    QList<ConfigPath> paths;
    QByteArray targetIdentity;
    // Logical declarations within an atomic record-slot target. Storage paths
    // remain the actual owner/base dependency cuts, never nested write routes.
    std::optional<QList<ConfigPath>> logicalPaths;
    QString previewId;
    std::shared_ptr<std::atomic_bool> previewLifetime;
    quint64 previewEpoch = 0;
};
struct ConfigObservationData {
    QList<ConfigObservationDependency> dependencies;
};
struct ConfigSelectedStateData {
    bool valid = false, targetPresent = false, candidateAvailable = false;
    QVariant effective, authored, candidate;
    ConfigSelectedState::Pending pending = ConfigSelectedState::Pending::None;
    QString maskedBy;
    ConfigObservation observation;
};
struct ConfigReadSnapshotData {
    ConfigSchema schema{ConfigDescriptor::group({})};
    std::shared_ptr<const ConfigSnapshot> admitted;
    QList<ConfigPath> selection;
    ConfigPath prefix;
    std::optional<QSet<ConfigPath>> targetPresence;
    QMap<ConfigPath, ConfigSelectedState> states;
    ConfigObservation observation;
};
struct ConfigReadResultData {
    quint64 requestId = 0;
    QString status;
    ConfigError error;
    ConfigReadSnapshot snapshot;
    QList<ConfigPath> pending;
};
struct ConfigCompletion {
    ConfigResult result;
    ConfigObservation observation;
};
// Tagged only inside std::function storage. Existing public Callback signatures
// remain unchanged; native/QML adapters forward this captured pair directly.
struct ConfigPairedCallback {
    std::function<void(const ConfigCompletion &)> callback;
    void operator()(const ConfigResult &result) const { callback({result, {}}); }
};
inline void invokeCompletion(const std::function<void(const ConfigResult &)> &callback,
                             const ConfigCompletion &pair) {
    if (const auto *paired = callback.target<ConfigPairedCallback>())
        paired->callback(pair);
    else if (callback)
        callback(pair.result);
}
struct ConfigReadRequest {
    QString projectionId, overlayId, previewId;
    ConfigSelection selection;
    ConfigPath prefix;
    quint64 requestId = 0;
    quint64 editEpoch = 0;
    ConfigObservation after;
    std::shared_ptr<std::atomic_bool> previewLifetime;
};
struct ConfigReadCapture {
    QString projectionId;
    quint64 requestId = 0, order = 0, coveredSequence = 0;
    ConfigReadSnapshot snapshot;
    QString error;
    quint64 editEpoch = 0;
};
class ConfigObservationAccess {
public:
    static ConfigObservation observation(QList<ConfigObservationDependency> dependencies);
    static const QList<ConfigObservationDependency> &dependencies(const ConfigObservation &token);
    static ConfigObservation withLogicalSelection(const ConfigObservation &token,
                                                  const QList<ConfigPath> &paths);
    // A weaker claim over a subset of an admitted token: ordinary storage cuts
    // keep only the selected paths; composed cuts narrow logical coverage.
    static ConfigObservation narrow(const ConfigObservation &token, const QList<ConfigPath> &paths);
    // The same dependency cuts without staged/preview scoping: the shape of the
    // coverage a committed write over this selection produces.
    static ConfigObservation committed(const ConfigObservation &token);
    static bool covers(const ConfigObservation &installed, const ConfigObservation &after);
    static bool applicable(const ConfigObservation &selection, const ConfigObservation &after);
    static ConfigSelectedState state(ConfigSelectedStateData data);
    static ConfigReadSnapshot snapshot(ConfigReadSnapshotData data);
    static ConfigReadResult result(ConfigReadResultData data);
    static ConfigReadSnapshot select(const ConfigReadSnapshot &snapshot,
                                     const QList<ConfigPath> &paths, const ConfigPath &prefix = {});
    static ConfigSchema schema(const ConfigReadSnapshot &snapshot);
    static ConfigReadSnapshot candidate(const ConfigCandidate &candidate,
                                        const ConfigSchema &schema,
                                        const ConfigObservation &observation);
    static ConfigReadSnapshot pending(const ConfigReadSnapshot &snapshot,
                                      const QMap<ConfigPath, std::optional<QVariant>> &edits);
};
} // namespace configqml
