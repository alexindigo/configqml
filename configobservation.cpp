// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configobservation_p.h"
#include "configoverlay_p.h"

namespace configqml {
bool ConfigObservation::empty() const { return !d || d->dependencies.isEmpty(); }
bool ConfigObservation::valid() const {
    if (empty())
        return false;
    for (const auto &dependency : d->dependencies) {
        const auto owner = dependency.owner.lock();
        if (!owner || !owner->live || owner->order != dependency.incarnation ||
            (dependency.previewLifetime && !*dependency.previewLifetime))
            return false;
    }
    return true;
}
bool ConfigObservation::operator==(const ConfigObservation &other) const { return d == other.d; }
bool ConfigSelectedState::valid() const { return d && d->valid; }
bool ConfigSelectedState::present() const { return d && d->effective.isValid(); }
QVariant ConfigSelectedState::value() const { return d ? d->effective : QVariant(); }
bool ConfigSelectedState::targetPresent() const { return d && d->targetPresent; }
ConfigSelectedState::Pending ConfigSelectedState::pending() const {
    return d ? d->pending : Pending::None;
}
bool ConfigSelectedState::authoredPresent() const { return d && d->authored.isValid(); }
QVariant ConfigSelectedState::authoredValue() const { return d ? d->authored : QVariant(); }
bool ConfigSelectedState::candidateAvailable() const { return d && d->candidateAvailable; }
bool ConfigSelectedState::candidatePresent() const { return d && d->candidate.isValid(); }
QVariant ConfigSelectedState::candidateValue() const { return d ? d->candidate : QVariant(); }
QString ConfigSelectedState::maskedBy() const { return d ? d->maskedBy : QString(); }
ConfigObservation ConfigSelectedState::observation() const {
    return d ? d->observation : ConfigObservation();
}
bool ConfigReadSnapshot::valid() const { return bool(d); }
QList<ConfigPath> ConfigReadSnapshot::paths() const {
    QList<ConfigPath> paths;
    if (d)
        for (const auto &path : d->selection)
            paths.append(path.mid(d->prefix.size()));
    return paths;
}
ConfigSelectedState ConfigReadSnapshot::selectedState(const ConfigPath &path) const {
    if (!d || !d->selection.contains(d->prefix + path))
        return {};
    const auto absolute = d->prefix + path;
    if (d->states.contains(absolute)) {
        auto state = *d->states.value(absolute).d;
        state.observation = d->observation;
        return ConfigObservationAccess::state(std::move(state));
    }
    if (!d->admitted)
        return {};
    ConfigSelectedStateData state;
    state.valid = true;
    state.effective = d->admitted->values.value(absolute);
    const auto info = d->admitted->information.value(absolute).provenance;
    state.maskedBy = info.maskedBy;
    if (d->targetPresence)
        state.targetPresent = d->targetPresence->contains(absolute);
    else
        for (const auto &contribution : info.contributions)
            if (contribution.layerId == info.route.layerId &&
                contribution.sourceId == info.route.sourceId)
                state.targetPresent = contribution.rawPresent;
    state.observation = d->observation;
    return ConfigObservationAccess::state(std::move(state));
}
ConfigObservation ConfigReadSnapshot::observation() const {
    return d ? d->observation : ConfigObservation();
}
quint64 ConfigReadResult::requestId() const { return d ? d->requestId : 0; }
QString ConfigReadResult::status() const { return d ? d->status : QString(); }
ConfigError ConfigReadResult::error() const { return d ? d->error : ConfigError(); }
ConfigObservation ConfigReadResult::observation() const { return snapshot().observation(); }
ConfigReadSnapshot ConfigReadResult::snapshot() const {
    return d ? d->snapshot : ConfigReadSnapshot();
}
QList<ConfigPath> ConfigReadResult::pending() const { return d ? d->pending : QList<ConfigPath>(); }
ConfigObservation
ConfigObservationAccess::observation(QList<ConfigObservationDependency> dependencies) {
    ConfigObservation token;
    token.d = std::make_shared<const ConfigObservationData>(
        ConfigObservationData{std::move(dependencies)});
    return token;
}
const QList<ConfigObservationDependency> &
ConfigObservationAccess::dependencies(const ConfigObservation &token) {
    static const QList<ConfigObservationDependency> empty;
    return token.d ? token.d->dependencies : empty;
}
ConfigObservation ConfigObservationAccess::withLogicalSelection(const ConfigObservation &token,
                                                                const QList<ConfigPath> &paths) {
    auto dependencies = ConfigObservationAccess::dependencies(token);
    for (auto &dependency : dependencies)
        if (!dependency.targetIdentity.isEmpty())
            dependency.logicalPaths = paths;
    return observation(std::move(dependencies));
}
namespace {
bool contains(const ConfigObservationDependency &have, const ConfigObservationDependency &need) {
    if (have.incarnation != need.incarnation ||
        (!need.previewId.isEmpty() &&
         (have.previewId != need.previewId || have.previewLifetime != need.previewLifetime)))
        return false;
    if ((!have.targetIdentity.isEmpty() && need.targetIdentity.isEmpty()) ||
        (!have.targetIdentity.isEmpty() && !need.targetIdentity.isEmpty() &&
         have.targetIdentity != need.targetIdentity))
        return false;
    for (const auto &path : need.paths)
        if (!have.paths.contains(path))
            return false;
    if (!have.targetIdentity.isEmpty() && need.logicalPaths) {
        if (!have.logicalPaths)
            return false;
        for (const auto &path : *need.logicalPaths)
            if (!have.logicalPaths->contains(path))
                return false;
    }
    return true;
}
} // namespace
bool ConfigObservationAccess::applicable(const ConfigObservation &selection,
                                         const ConfigObservation &after) {
    if (after.empty())
        return true;
    if (!after.valid() || !selection.valid())
        return false;
    for (const auto &need : dependencies(after)) {
        bool found = false;
        for (const auto &have : dependencies(selection))
            found |= contains(have, need);
        if (!found)
            return false;
    }
    return true;
}
bool ConfigObservationAccess::covers(const ConfigObservation &installed,
                                     const ConfigObservation &after) {
    if (!applicable(installed, after))
        return false;
    for (const auto &need : dependencies(after)) {
        bool found = false;
        for (const auto &have : dependencies(installed))
            found |= contains(have, need) && have.delivery >= need.delivery &&
                     have.previewEpoch >= need.previewEpoch;
        if (!found)
            return false;
    }
    return true;
}
ConfigSelectedState ConfigObservationAccess::state(ConfigSelectedStateData data) {
    ConfigSelectedState state;
    state.d = std::make_shared<const ConfigSelectedStateData>(std::move(data));
    return state;
}
ConfigReadSnapshot ConfigObservationAccess::snapshot(ConfigReadSnapshotData data) {
    ConfigReadSnapshot snapshot;
    snapshot.d = std::make_shared<const ConfigReadSnapshotData>(std::move(data));
    return snapshot;
}
ConfigReadResult ConfigObservationAccess::result(ConfigReadResultData data) {
    ConfigReadResult result;
    result.d = std::make_shared<const ConfigReadResultData>(std::move(data));
    return result;
}
ConfigSchema ConfigObservationAccess::schema(const ConfigReadSnapshot &snapshot) {
    if (!snapshot.d)
        return ConfigSchema(ConfigDescriptor::group({}));
    if (snapshot.d->prefix.isEmpty())
        return snapshot.d->schema;
    return ConfigSchema(
        snapshot.d->schema.descriptor(snapshot.d->prefix).value_or(ConfigDescriptor::group({})));
}
ConfigReadSnapshot ConfigObservationAccess::candidate(const ConfigCandidate &candidate,
                                                      const ConfigSchema &schema,
                                                      const ConfigObservation &observation) {
    if (!candidate.valid())
        return {};
    ConfigReadSnapshotData data;
    data.schema = schema;
    data.selection = candidate.facts.keys();
    data.observation = observation;
    for (auto it = candidate.facts.cbegin(); it != candidate.facts.cend(); ++it) {
        ConfigSelectedStateData state;
        state.valid = true;
        state.effective = it->effective;
        state.targetPresent = it->targetPresent;
        state.pending = !it->edited ? ConfigSelectedState::Pending::None
                        : it->pending == ConfigEdit::Kind::Set
                            ? ConfigSelectedState::Pending::Set
                            : ConfigSelectedState::Pending::Remove;
        state.authored = it->authored;
        state.candidateAvailable = true;
        state.candidate = it->effective;
        state.maskedBy = it->maskedBy;
        state.observation = observation;
        data.states[it.key()] = ConfigObservationAccess::state(std::move(state));
    }
    return ConfigObservationAccess::snapshot(std::move(data));
}
ConfigReadSnapshot ConfigObservationAccess::select(const ConfigReadSnapshot &snapshot,
                                                   const QList<ConfigPath> &paths,
                                                   const ConfigPath &prefix) {
    if (!snapshot.d)
        return {};
    const bool sameSelection = paths == snapshot.d->selection;
    if (sameSelection && prefix == snapshot.d->prefix)
        return snapshot; // The already immutable capture is the root projection.
    auto data = *snapshot.d;
    if (!sameSelection) {
        data.selection.clear();
        const QSet<ConfigPath> allowed(snapshot.d->selection.cbegin(),
                                       snapshot.d->selection.cend());
        for (const auto &path : paths)
            if (allowed.contains(path))
                data.selection.append(path);
    }
    data.prefix = prefix;
    auto dependencies = ConfigObservationAccess::dependencies(data.observation);
    if (!sameSelection) {
        // Ordinary projections select actual declared storage leaves. Composed
        // projections narrow logical coverage without rewriting atomic cuts.
        if (dependencies.size() == 1 && dependencies.first().targetIdentity.isEmpty())
            dependencies.first().paths = data.selection;
        data.observation =
            withLogicalSelection(observation(std::move(dependencies)), data.selection);
    }
    return ConfigObservationAccess::snapshot(std::move(data));
}
ConfigReadSnapshot
ConfigObservationAccess::pending(const ConfigReadSnapshot &snapshot,
                                 const QMap<ConfigPath, std::optional<QVariant>> &edits) {
    if (!snapshot.d)
        return {};
    auto data = *snapshot.d;
    for (const auto &path : data.selection) {
        const auto relative = path.mid(data.prefix.size());
        const auto captured = snapshot.selectedState(relative);
        if (!captured.d)
            continue;
        auto state = *captured.d;
        const auto edit = edits.constFind(path);
        state.pending = edit == edits.cend() ? ConfigSelectedState::Pending::None
                        : edit.value()       ? ConfigSelectedState::Pending::Set
                                             : ConfigSelectedState::Pending::Remove;
        state.authored = edit == edits.cend() ? QVariant() : edit.value().value_or(QVariant());
        state.candidateAvailable = false;
        state.candidate = {};
        data.states[path] = ConfigObservationAccess::state(std::move(state));
    }
    return ConfigObservationAccess::snapshot(std::move(data));
}
} // namespace configqml
