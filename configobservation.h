// SPDX-License-Identifier: LGPL-3.0-or-later
// Immutable observation facts shared by compatibility projections and handles.
#pragma once
#include "configresult.h"
#include <memory>

namespace configqml {
struct ConfigObservationData;
struct ConfigSelectedStateData;
struct ConfigReadSnapshotData;
struct ConfigReadResultData;
class ConfigObservationAccess;
class ConfigObservation {
public:
    ConfigObservation() = default;
    bool empty() const;
    bool valid() const;
    bool operator==(const ConfigObservation &other) const;
    bool operator!=(const ConfigObservation &other) const { return !(*this == other); }

private:
    friend class ConfigObservationAccess;
    std::shared_ptr<const ConfigObservationData> d;
};
class ConfigSelectedState {
public:
    enum class Pending { None, Set, Remove };
    ConfigSelectedState() = default;
    bool valid() const;
    bool present() const;
    QVariant value() const;
    bool targetPresent() const;
    Pending pending() const;
    bool authoredPresent() const;
    QVariant authoredValue() const;
    bool candidateAvailable() const;
    bool candidatePresent() const;
    QVariant candidateValue() const;
    QString maskedBy() const;
    ConfigObservation observation() const;

private:
    friend class ConfigObservationAccess;
    friend class ConfigReadSnapshot;
    std::shared_ptr<const ConfigSelectedStateData> d;
};
// Immutable selected facts, not a facade or a later live getter.
class ConfigReadSnapshot {
public:
    ConfigReadSnapshot() = default;
    bool valid() const;
    QList<ConfigPath> paths() const;
    ConfigSelectedState selectedState(const ConfigPath &path) const;
    ConfigObservation observation() const;

private:
    friend class ConfigObservationAccess;
    std::shared_ptr<const ConfigReadSnapshotData> d;
};
class ConfigReadResult {
public:
    ConfigReadResult() = default;
    quint64 requestId() const;
    QString status() const;
    ConfigError error() const;
    ConfigObservation observation() const;
    ConfigReadSnapshot snapshot() const;
    QList<ConfigPath> pending() const;

private:
    friend class ConfigObservationAccess;
    std::shared_ptr<const ConfigReadResultData> d;
};
} // namespace configqml
Q_DECLARE_METATYPE(configqml::ConfigObservation)
Q_DECLARE_METATYPE(configqml::ConfigSelectedState)
Q_DECLARE_METATYPE(configqml::ConfigReadSnapshot)
Q_DECLARE_METATYPE(configqml::ConfigReadResult)
