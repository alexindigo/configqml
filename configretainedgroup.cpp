// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "configshutdown_p.h"
#include <QUuid>
#include <QCryptographicHash>
#include <algorithm>

namespace configqml {
namespace {
bool selected(const QList<ConfigPath> &paths, const ConfigPath &path) {
    for (const auto &prefix : paths)
        if (startsWith(path, prefix))
            return true;
    return false;
}
QString opaqueId() { return QUuid::createUuid().toString(QUuid::Id128); }
QByteArray commandBody(const ConfigRetainedGroupOptions &options) {
    QVariantList paths, edits;
    for (const auto &path : options.paths)
        paths.append(QVariant(path));
    for (const auto &edit : options.edits) {
        QVariantMap value{{"path", edit.path}, {"present", edit.present}};
        if (edit.present)
            value["value"] = edit.value;
        edits.append(value);
    }
    return QCryptographicHash::hash(
        encodeMemoryValue(
            {{"paths", paths}, {"edits", edits}, {"dependencies", options.dependencies}}),
        QCryptographicHash::Sha256);
}
bool currentGeneration(const ConfigCommand &command, const ConfigRetainedCapture &capture) {
    // A not-yet-published initial capture has generation zero. Every subsequent
    // lifecycle transition is fenced independently of settings mutation input.
    return command.groupGenerations.value(capture.info.groupToken) == capture.generation;
}
} // namespace
QMap<QString, quint64> ConfigStore::retainedGenerations() const {
    QMap<QString, quint64> generations;
    for (auto it = m_retainedIngress.cbegin(); it != m_retainedIngress.cend(); ++it)
        generations[it.key()] = qMax(it->generation, it->pendingGeneration);
    return generations;
}
ConfigGroupInfo ConfigStore::groupInfo(const QString &token) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_groups.contains(token))
        return m_groups.value(token);
    ConfigGroupInfo result;
    result.error = {"group-token", {}, "groupInfo"};
    return result;
}
quint64 ConfigStore::retainGroup(const ConfigRetainedGroupOptions &options, QObject *context,
                                 Callback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto id = registerRequest(context, std::move(callback));
    ConfigCommand command{id, m_sequence, "retainGroup", {}};
    command.retainedGroup = options;
    auto &owned = *command.retainedGroup;
    owned.requestKey = ownString(options.requestKey);
    for (auto &path : owned.paths)
        path = ownPath(path);
    for (auto &dependency : owned.dependencies)
        dependency = ownString(dependency);
    ConfigInput input;
    input.kind = ConfigInput::Kind::Group;
    input.sequence = m_sequence + (options.edits.isEmpty() ? 0 : 1);
    for (auto &edit : owned.edits) {
        edit.path = ownPath(edit.path);
        const auto captured =
            edit.present ? m_schema.validate(edit.path, edit.value) : ConfigValueResult{};
        if (!captured.ok())
            command.conversionError = captured.errors.first().code;
        edit.value = captured.value;
        input.edits[edit.path] =
            edit.present ? std::optional<QVariant>(captured.value) : std::nullopt;
        for (const auto &leaf : m_schema.leaves())
            if (startsWith(leaf, edit.path))
                input.pathGenerations[leaf] = info(leaf).pathGeneration + 1;
    }
    if (!owned.edits.isEmpty()) {
        command.prefix = input.sequence;
        command.input = input;
    }
    if (!m_accepting || m_status != Status::Ready)
        command.conversionError = "store-closed";
    if (!command.conversionError.isEmpty()) {
        command.input.reset();
        command.prefix = m_sequence;
    }
    // Claim request identity before mutation ingress, not after a worker reply.
    // Reentries/queued duplicates are lookups even while the original is pending.
    // The worker still compares the complete checked body and dependencies.
    if (command.conversionError.isEmpty() && m_retainedRequests.contains(owned.requestKey)) {
        command.retainedLookup = true;
        command.input.reset();
        command.prefix = m_sequence;
    }
    command.groupGenerations = retainedGenerations();
    if (!m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = command.operation;
        result.status = "failed";
        result.error = {"store-closed", {}, command.operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    } else {
        if (command.conversionError.isEmpty() && !command.retainedLookup)
            m_retainedRequests.insert(owned.requestKey, id);
        if (command.input) {
            m_sequence = input.sequence;
            m_inputs.append(input);
            for (auto it = input.pathGenerations.cbegin(); it != input.pathGenerations.cend();
                 ++it) {
                m_cache.information[it.key()].latestInput = input.sequence;
                m_cache.information[it.key()].pathGeneration = it.value();
            }
        }
    }
    return id;
}
quint64 ConfigStore::groupRequest(const QString &operation, const QString &token, QObject *context,
                                  Callback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto id = registerRequest(context, std::move(callback));
    ConfigCommand command{id, m_sequence, operation, {}};
    command.groupToken = ownString(token);
    command.groupGenerations = retainedGenerations();
    const auto ingress = m_retainedIngress.value(token);
    command.groupState = ingress.pendingRequest ? ingress.pendingState : ingress.state;
    if (m_retainedIngress.contains(token))
        command.prefix = ingress.capturePrefix;
    if (!m_accepting)
        command.conversionError = "store-closed";
    if (!m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = operation;
        result.status = "failed";
        result.error = {"store-closed", {}, operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    } else if (command.conversionError.isEmpty()) {
        const auto expected = operation == "releaseGroup" ? QString("succeeded")
                              : operation == "retryGroup" ? QString("stopped")
                                                          : QString("retained");
        if (command.groupState == expected) {
            auto &claim = m_retainedIngress[command.groupToken];
            claim.pendingRequest = id;
            claim.pendingGeneration = command.groupGenerations.value(token) + 1;
            claim.pendingState = operation == "releaseGroup" ? "released" : "saving";
        }
    }
    return id;
}
quint64 ConfigStore::saveGroup(const QString &token, QObject *context, Callback callback) {
    return groupRequest("saveGroup", token, context, std::move(callback));
}
quint64 ConfigStore::retryGroup(const QString &token, QObject *context, Callback callback) {
    return groupRequest("retryGroup", token, context, std::move(callback));
}
quint64 ConfigStore::releaseGroup(const QString &token, QObject *context, Callback callback) {
    return groupRequest("releaseGroup", token, context, std::move(callback));
}
void ConfigWorker::describeGroup(ConfigResult &result, const ConfigGroupInfo &info) const {
    result.groupToken = info.groupToken;
    result.requestKey = info.requestKey;
    result.capturePrefix = info.capturePrefix;
    result.coverage = info.coverage;
    result.dependencies = info.dependencies;
    if (!info.attempts.isEmpty()) {
        result.attemptId = info.attempts.last().attemptId;
        result.attemptNumber = info.attempts.last().attemptNumber;
    }
}
QStringList ConfigWorker::unresolvedGroups(quint64 prefix) const {
    QStringList result;
    for (const auto &capture : m_groups)
        if (capture.info.capturePrefix <= prefix && capture.info.state != "succeeded" &&
            capture.info.state != "released")
            result.append(capture.info.groupToken);
    return result;
}
bool ConfigWorker::reserved(const ConfigPath &path) const {
    for (const auto &capture : m_groups)
        if (capture.info.state != "succeeded" && capture.info.state != "released")
            for (const auto &selection : capture.selections)
                if (startsWith(path, selection) || startsWith(selection, path))
                    return true;
    return false;
}
void ConfigWorker::refreshDirty(const QString &source) {
    quint64 prefix = 0;
    for (const auto &intent : m_intents.value(source))
        prefix = qMax(prefix, intent.input);
    if (m_intents.value(source).isEmpty()) {
        m_dirty.remove(source);
        m_pendingRevisions.remove(source);
        m_pendingBytes.remove(source);
    } else
        m_dirty[source] = prefix;
}
QList<ConfigFileEdit> ConfigWorker::pendingEdits(const QString &source) const {
    QList<ConfigFileEdit> edits;
    for (const auto &intent : m_intents.value(source))
        edits.append(intent.edit);
    return edits;
}
QList<ConfigPendingIntent> ConfigWorker::eligibleIntents(const QString &source,
                                                         const ConfigCommand &command) const {
    QList<ConfigPendingIntent> result;
    for (const auto &intent : m_intents.value(source)) {
        if (!command.groupToken.isEmpty()) {
            if (m_groups.value(command.groupToken).intents.value(source).contains(intent.id))
                result.append(intent);
        } else if (intent.input <= command.prefix && intent.reservation.isEmpty() &&
                   (intent.edit.pruneEmpty || !reserved(intent.path)))
            result.append(intent);
    }
    return result;
}
void ConfigWorker::recordPersistentEdit(const ConfigRoute &route, const ConfigPath &path,
                                        const std::optional<QVariant> &value, bool ifMissing,
                                        quint64 input, const ConfigLayers &before) {
    if (!route.valid())
        return;
    if (!m_dirty.contains(route.sourceId)) {
        m_pendingRevisions[route.sourceId] = m_revisions.value(route.sourceId);
        m_pendingBytes[route.sourceId] = m_sourceBytes.value(route.sourceId);
    }
    const auto relative = path.mid(route.prefix.size());
    auto append = [&](const ConfigPath &absolute, const std::optional<QVariant> &v,
                      const std::optional<ConfigPath> &fence, bool prune) {
        ConfigFileEdit edit{absolute.mid(route.prefix.size()), v, ifMissing};
        edit.fencePath = fence;
        edit.pruneEmpty = prune;
        m_intents[route.sourceId].append({++m_nextIntent, input, absolute, edit, {}});
    };
    const auto descriptor = m_options.schema.descriptor(path);
    if (!value && descriptor && descriptor->type == ConfigDescriptor::Type::Group) {
        const auto source = before.source(route.layerId, route.sourceId);
        const auto raw = source ? valueAt(source->raw, relative).value_or(QVariantMap{})
                                : QVariant(QVariantMap{});
        std::function<void(const ConfigDescriptor &, const ConfigPath &, const QVariant &)> expand;
        expand = [&](const ConfigDescriptor &node, const ConfigPath &absolute,
                     const QVariant &original) {
            if (node.type != ConfigDescriptor::Type::Group ||
                (original.isValid() && original.metaType().id() != QMetaType::QVariantMap)) {
                append(absolute, {}, relative, false);
                return;
            }
            const auto map = original.toMap();
            for (auto child = node.fields.cbegin(); child != node.fields.cend(); ++child)
                expand(*child, absolute + ConfigPath{child.key()}, map.value(child.key()));
            for (auto child = map.cbegin(); child != map.cend(); ++child)
                if (!node.fields.contains(child.key()))
                    append(absolute + ConfigPath{child.key()}, {}, relative, false);
            append(absolute, {}, relative, true);
        };
        expand(*descriptor, path, raw);
    } else {
        append(path, value, {}, false);
        if (!value && m_files.value(route.sourceId).deleteWhenEmpty)
            for (qsizetype n = path.size() - 1; n > route.prefix.size(); --n) {
                const auto parent = path.mid(0, n);
                const auto node = m_options.schema.descriptor(parent);
                if (node && node->type == ConfigDescriptor::Type::Group)
                    append(parent, {}, relative, true);
            }
    }
    refreshDirty(route.sourceId);
}
void ConfigWorker::recordPersistent(const ConfigInput &input, const ConfigLayers &before) {
    if (input.kind == ConfigInput::Kind::Group) {
        for (auto edit = input.edits.cbegin(); edit != input.edits.cend(); ++edit)
            for (const auto &route : m_routes)
                if (startsWith(edit.key(), route.prefix) || startsWith(route.prefix, edit.key()))
                    recordPersistentEdit(
                        route, startsWith(route.prefix, edit.key()) ? route.prefix : edit.key(),
                        edit.value(), false, input.sequence, before);
    } else
        for (const auto &route : m_routes)
            if (startsWith(input.path, route.prefix) || startsWith(route.prefix, input.path))
                recordPersistentEdit(
                    route, startsWith(route.prefix, input.path) ? route.prefix : input.path,
                    input.kind == ConfigInput::Kind::Reset ? std::nullopt
                                                           : std::optional<QVariant>(input.value),
                    input.kind == ConfigInput::Kind::Initialize, input.sequence, before);
}
void ConfigWorker::confirmed(const QString &id, const QVariantMap &raw,
                             const ConfigBackendResult &outcome) {
    const auto saved = m_saveIntents.value(id);
    QSet<quint64> ids;
    for (const auto &intent : saved)
        if (!intent.edit.pruneEmpty || !reserved(intent.path) || !valueAt(raw, intent.edit.path) ||
            valueAt(raw, intent.edit.path)->toMap().isEmpty())
            ids.insert(intent.id);
    auto &remaining = m_intents[id];
    remaining.removeIf([&](const ConfigPendingIntent &intent) { return ids.contains(intent.id); });
    m_baselines[id] = raw;
    m_revisions[id] = outcome.revision;
    m_pendingRevisions[id] = outcome.revision;
    m_sourceBytes[id] = outcome.bytes;
    m_pendingBytes[id] = outcome.bytes;
    for (auto &capture : m_groups)
        if (capture.bases.contains(id)) {
            if (capture.info.groupToken == m_save.groupToken)
                capture.confirmedRevisions[id] = outcome.revision;
            capture.bases[id] = raw;
            capture.revisions[id] = outcome.revision;
            capture.bytes[id] = outcome.bytes;
            capture.intents[id].subtract(ids);
            if (capture.uncertain.contains(id)) {
                if (capture.info.groupToken == m_save.groupToken) {
                    capture.uncertain.remove(id);
                    capture.uncertainBytes.remove(id);
                } else {
                    capture.uncertain[id] = outcome.revision;
                    capture.uncertainBytes[id] = outcome.bytes;
                }
            }
        }
    const auto route = m_routes.value(id);
    auto disk = *m_layers->source(route.layerId, id);
    disk.raw = raw;
    disk.bytes = outcome.bytes;
    disk.revision = outcome.revision.token;
    disk.exists = outcome.revision.exists;
    disk.diskExists = disk.exists;
    disk.localIntent = false;
    m_diskLayers->replaceSource(route.layerId, disk);
    for (const auto &intent : remaining)
        applyFileEdit(disk.raw, intent.edit);
    disk.localIntent = !remaining.isEmpty();
    disk.exists |= disk.localIntent;
    m_layers->replaceSource(route.layerId, disk);
    refreshDirty(id);
    m_acceptOrigin = "own_save";
    m_acceptCause = "save";
}
bool ConfigWorker::recoverOwnPublication(const QString &source, const ConfigBackendResult &read) {
    QString error;
    if (m_files.contains(source))
        error = retainObservation(source, read, "readback", "late", m_save, &m_saveResult);
    for (const auto &capture : m_groups)
        if (capture.uncertain.contains(source) &&
            (!read.success || capture.uncertain.value(source) != read.revision ||
             capture.uncertainBytes.value(source) != read.bytes))
            error = read.success ? QString("source-preimage-conflict") : read.error.code;
    if (!error.isEmpty()) {
        m_saveResult.status = "failed";
        m_saveResult.error = {error, source, "readback"};
        m_saveResult.publications.append(
            {source, false, read.revision, "known", m_saveResult.error});
        return false;
    }
    ConfigJsonCodec codec(m_options.schema.diagnostics().at(m_routes.value(source).prefix));
    const auto raw = m_files.contains(source) ? codec.decode(read.bytes, source).raw
                                              : decodeMemoryValue(read.bytes);
    m_saveBases[source] = raw;
    m_saveRevisions[source] = read.revision;
    m_saveBytes[source] = read.bytes;
    m_recoveryWhole.insert(source);
    return true;
}
void ConfigWorker::retainedGroupCommand(const ConfigCommand &command) {
    auto result = resultFor(command);
    auto fail = [&](const QString &code) {
        coverInput(command);
        result.status = "failed";
        result.error = {code, {}, command.operation};
        post(publish(result));
    };
    if (!command.conversionError.isEmpty()) {
        fail(command.conversionError);
        return;
    }
    if (command.retainedGroup) {
        const auto &options = *command.retainedGroup;
        const auto body = commandBody(options);
        if (m_groupKeys.contains(options.requestKey)) {
            const auto &capture = m_groups[m_groupKeys.value(options.requestKey)];
            if (!currentGeneration(command, capture)) {
                fail("group-state");
                return;
            }
            if (capture.commandBody != body) {
                fail("group-key-mismatch");
                return;
            }
            result.status = "retained";
            describeGroup(result, capture.info);
            post(publish(result));
            return;
        }
        // An earlier pending claim may have failed semantic admission. A lookup
        // must never turn into a fresh, unsequenced mutation in that case.
        if (command.retainedLookup) {
            fail("group-state");
            return;
        }
        QSet<ConfigPath> selections;
        if (options.requestKey.isEmpty() || options.paths.isEmpty()) {
            fail("group-selection");
            return;
        }
        QList<ConfigPath> leaves;
        for (const auto &selection : options.paths) {
            if (selections.contains(selection) || !m_options.schema.descriptor(selection)) {
                fail("group-selection");
                return;
            }
            selections.insert(selection);
            for (const auto &path : m_options.schema.leaves())
                if (startsWith(path, selection) && !leaves.contains(path)) {
                    if (!m_layers->info(path).route.valid()) {
                        fail("group-selection");
                        return;
                    }
                    if (!quarantined(path).isEmpty()) {
                        fail("source-quarantined");
                        return;
                    }
                    leaves.append(path);
                }
        }
        if (leaves.isEmpty()) {
            fail("group-selection");
            return;
        }
        QSet<QString> dependencies;
        for (const auto &dependency : options.dependencies) {
            if (!m_groups.contains(dependency) || dependencies.contains(dependency)) {
                fail("group-dependency");
                return;
            }
            if (!currentGeneration(command, m_groups.value(dependency))) {
                fail("group-state");
                return;
            }
            dependencies.insert(dependency);
        }
        QMap<ConfigPath, std::optional<QVariant>> edits;
        for (const auto &edit : options.edits) {
            if (!selected(options.paths, edit.path) || !m_options.schema.descriptor(edit.path)) {
                fail("group-selection");
                return;
            }
            for (auto prior = edits.cbegin(); prior != edits.cend(); ++prior)
                if (startsWith(prior.key(), edit.path) || startsWith(edit.path, prior.key())) {
                    fail("group-selection");
                    return;
                }
            if (!quarantined(edit.path).isEmpty()) {
                fail("source-quarantined");
                return;
            }
            edits[edit.path] = edit.present ? std::optional<QVariant>(edit.value) : std::nullopt;
        }
        auto candidate = *m_layers;
        QSet<QString> ancestry = dependencies;
        QStringList pending = options.dependencies;
        while (!pending.isEmpty()) {
            const auto dependency = pending.takeFirst();
            for (const auto &ancestor : m_groups.value(dependency).info.dependencies)
                if (!ancestry.contains(ancestor)) {
                    ancestry.insert(ancestor);
                    pending.append(ancestor);
                }
        }
        for (const auto &prior : m_groups)
            for (const auto &selection : options.paths)
                for (const auto &earlier : prior.selections)
                    if (startsWith(selection, earlier) || startsWith(earlier, selection)) {
                        if (!currentGeneration(command, prior)) {
                            fail("group-state");
                            return;
                        }
                        if (prior.info.state != "succeeded" && prior.info.state != "released" &&
                            !ancestry.contains(prior.info.groupToken)) {
                            fail("group-dependency");
                            return;
                        }
                    }
        if (m_options.admission)
            for (auto edit = edits.cbegin(); edit != edits.cend(); ++edit) {
                ConfigInput input;
                input.kind = edit.value() ? ConfigInput::Kind::User : ConfigInput::Kind::Reset;
                input.path = edit.key();
                input.value = edit.value().value_or(QVariant());
                input.sequence = command.prefix;
                const auto error = m_options.admission(input);
                if (!error.isEmpty()) {
                    fail(error);
                    return;
                }
            }
        const auto admission = candidate.applyEdits(edits);
        if (!admission.accepted) {
            fail(admission.errors.isEmpty() ? QString("group-selection")
                                            : admission.errors.first().code);
            return;
        }
        const auto token = opaqueId();
        ConfigRetainedCapture capture;
        capture.info.groupToken = token;
        capture.info.requestKey = options.requestKey;
        capture.info.state = "retained";
        capture.info.capturePrefix = command.prefix;
        capture.info.dependencies = options.dependencies;
        capture.commandBody = body;
        capture.selections = options.paths;
        const auto before = *m_layers;
        *m_layers = std::move(candidate);
        coverInput(command);
        if (command.input) {
            auto input = *command.input;
            input.edits = edits;
            acceptIntent(input);
            recordPersistent(input, before);
        }
        for (auto source = m_intents.begin(); source != m_intents.end(); ++source)
            for (auto &intent : source.value())
                if (intent.reservation.isEmpty() && intent.input <= command.prefix &&
                    (selected(options.paths, intent.path) ||
                     (intent.edit.pruneEmpty &&
                      std::any_of(options.paths.cbegin(), options.paths.cend(),
                                  [&](const ConfigPath &selection) {
                                      return startsWith(selection, intent.path);
                                  })))) {
                    if (!intent.edit.pruneEmpty)
                        intent.reservation = token;
                    capture.intents[source.key()].insert(intent.id);
                    capture.bases[source.key()] = m_baselines.value(source.key());
                    capture.revisions[source.key()] =
                        m_pendingRevisions.value(source.key(), m_revisions.value(source.key()));
                    capture.bytes[source.key()] =
                        m_pendingBytes.value(source.key(), m_sourceBytes.value(source.key()));
                }
        for (const auto &path : leaves) {
            const auto id = m_layers->info(path).route.sourceId;
            if (!capture.bases.contains(id)) {
                capture.bases[id] = m_baselines.value(id);
                capture.revisions[id] = m_pendingRevisions.value(id, m_revisions.value(id));
                capture.bytes[id] = m_pendingBytes.value(id, m_sourceBytes.value(id));
            }
            capture.info.coverage.append({path, m_layers->info(path).route.sourceId,
                                          m_pathInputs.value(path), m_pathIntents.value(path)});
        }
        capture.originalBases = capture.bases;
        capture.originalRevisions = capture.revisions;
        capture.originalBytes = capture.bytes;
        m_groups.insert(token, capture);
        m_groupKeys.insert(options.requestKey, token);
        result.status = "retained";
        describeGroup(result, capture.info);
        post(publish(result));
        scheduleAutoSave();
        return;
    }
    if (!m_groups.contains(command.groupToken)) {
        fail("group-token");
        return;
    }
    auto &capture = m_groups[command.groupToken];
    describeGroup(result, capture.info);
    if (!currentGeneration(command, capture)) {
        fail("group-state");
        return;
    }
    if (command.operation == "releaseGroup") {
        if (capture.info.state != "succeeded" || command.groupState != "succeeded") {
            fail("group-state");
            return;
        }
        capture.info.state = "released";
        ++capture.generation;
        capture.bases.clear();
        capture.revisions.clear();
        capture.intents.clear();
        capture.uncertain.clear();
        capture.uncertainBytes.clear();
        capture.originalBases.clear();
        capture.originalRevisions.clear();
        capture.bytes.clear();
        capture.originalBytes.clear();
        result.status = "released";
        post(publish(result));
        return;
    }
    const auto expected =
        command.operation == "retryGroup" ? QString("stopped") : QString("retained");
    if (capture.info.state != expected || command.groupState != expected) {
        fail("group-state");
        return;
    }
    for (const auto &dependency : capture.info.dependencies)
        if (!currentGeneration(command, m_groups.value(dependency))) {
            fail("group-state");
            return;
        }
    capture.info.state = "saving";
    ++capture.generation;
    capture.info.attempts.append({opaqueId(),
                                  quint64(capture.info.attempts.size() + 1),
                                  command.requestId,
                                  {},
                                  "known",
                                  {}});
    post(publish());
    for (const auto &dependency : capture.info.dependencies)
        if (m_groups.value(dependency).info.state != "succeeded" &&
            m_groups.value(dependency).info.state != "released") {
            m_save = command;
            m_saveResult = resultFor(command);
            describeGroup(m_saveResult, capture.info);
            m_saveResult.status = "failed";
            m_saveResult.error = {"group-dependency-stopped", {}, command.operation};
            m_saveRetainedFailure = false;
            m_unresolvedSave = false;
            finishSave();
            return;
        }
    beginSave(command);
}
} // namespace configqml
