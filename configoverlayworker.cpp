// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include <QUuid>

namespace configqml {
namespace {
bool overlaps(const ConfigPath &a, const ConfigPath &b) {
    return startsWith(a, b) || startsWith(b, a);
}
bool sameFence(const ConfigPathState &a, const ConfigPathState &b) {
    return a.contentRevision == b.contentRevision && a.intentRevision == b.intentRevision &&
           a.pathGeneration == b.pathGeneration &&
           a.publicationGeneration == b.publicationGeneration;
}
void release(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    for (const auto &endpoint : t->order)
        endpoint->send([token = t->token](ConfigWorker &worker) { worker.overlayRelease(token); });
}
void advance(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    if (t->cancelled)
        t->error = "overlay-owner";
    if (!t->error.isEmpty() || t->acquired == t->order.size()) {
        if (!t->selection.owner->send([t](ConfigWorker &worker) { worker.overlayContinue(t); }))
            release(t);
        return;
    }
    if (!t->order[t->acquired]->send([t](ConfigWorker &worker) { worker.overlayLease(t); })) {
        t->error = "overlay-owner";
        advance(t);
    }
}
ConfigOverlayPresentation presentation(const QString &id, const ConfigOverlayCapture &capture) {
    ConfigOverlayPresentation p;
    p.editorId = id;
    p.mode = capture.selection.mode;
    p.schema = capture.schema;
    p.ownerPath = capture.selection.ownerPath;
    const auto merged = overlayMerge(capture.baseValues, capture.overrides).toMap();
    for (const auto &path : p.schema.leaves()) {
        const auto value = valueAt(merged, path);
        if (value) {
            const auto checked = p.schema.validate(path, *value);
            if (!checked.ok()) {
                p.error = checked.errors.first().code;
                return p;
            }
            p.values[path] = checked.value;
        } else
            p.values[path] = {};
    }
    return p;
}
} // namespace
QList<ConfigPath> ConfigWorker::commandPaths(const ConfigCommand &command) const {
    if (command.operation == "overlayLease" && command.overlay) {
        const auto path = m_endpoint == command.overlay->selection.owner
                              ? command.overlay->selection.ownerPath
                              : command.overlay->selection.basePath;
        QList<ConfigPath> paths{path};
        for (const auto &route : m_fileRoutes)
            if (overlaps(path, route.prefix))
                paths.append(route.prefix);
        return paths;
    }
    if (command.operation == "openOverlay" && command.overlay)
        return {command.overlay->selection.ownerPath};
    if (command.input) {
        auto paths = command.input->pathGenerations.keys();
        if (paths.isEmpty() && !command.input->path.isEmpty())
            paths.append(command.input->path);
        for (auto it = command.input->edits.cbegin(); it != command.input->edits.cend(); ++it)
            paths.append(it.key());
        return paths;
    }
    if (command.operation == "updateRecordListPolicy")
        return {m_listPolicies.value(command.policyId).path};
    if (command.operation.startsWith("overlay") && m_overlayCaptures.contains(command.editorId))
        return {m_overlayCaptures.value(command.editorId).selection.ownerPath};
    return {{}}; // Global reads, profiles, persistence and terminal commands preserve ordering.
}
bool ConfigWorker::overlayBlocked(const ConfigCommand &command) const {
    if ((command.operation == "close" || command.operation == "retire") &&
        !m_overlayTransactions.isEmpty())
        return true;
    const auto paths = commandPaths(command);
    for (const auto &lease : m_overlayLeases)
        for (const auto &selected : lease)
            for (const auto &path : paths)
                if (overlaps(path, selected))
                    return true;
    // A bypass may never overtake an earlier command touching the same path.
    for (const auto &earlier : m_queue) {
        if (&earlier == &command)
            break;
        const auto selected = commandPaths(earlier);
        for (const auto &a : selected)
            for (const auto &b : paths)
                if (overlaps(a, b))
                    return true;
    }
    return false;
}
void ConfigWorker::overlayLease(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    ConfigCommand command;
    command.operation = "overlayLease";
    command.overlay = t;
    enqueue(command);
}
void ConfigWorker::overlayRelease(const QString &token) {
    m_overlayLeases.remove(token);
    m_overlayIncoming.remove(token);
    const auto paused = m_overlayPaused.value(token);
    if (paused && paused->cancelled)
        advance(m_overlayPaused.take(token));
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        const auto pending = m_queue[i].overlay;
        if (m_queue[i].operation == "overlayLease" && pending && pending->token == token &&
            pending->cancelled) {
            auto transaction = m_queue.takeAt(i).overlay;
            advance(transaction);
            break;
        }
    }
    drain();
}
void ConfigWorker::overlayAcquire(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    try {
        overlayAcquireImpl(t);
    } catch (...) {
        m_reloadSelection.clear();
        fail("worker-exception");
        t->error = "worker-exception";
        advance(t);
    }
}
void ConfigWorker::overlayAcquireImpl(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    const auto &s = t->selection;
    const bool main = m_endpoint == s.owner;
    const auto paths = main ? QList<ConfigPath>{s.ownerPath} : QList<ConfigPath>{s.basePath};
    if (t->cancelled || !s.owner->live || !s.base->live || !s.baseLifetime || !*s.baseLifetime ||
        m_failed || m_closed) {
        t->error = "overlay-owner";
        advance(t);
        return;
    }
    m_reloadSelection = paths;
    const bool preflight =
        m_files.isEmpty() || reloadFiles(ConfigCommand{0, m_prefix, "reload", {}});
    m_reloadSelection.clear();
    if (!preflight) {
        t->error = "overlay-conflict";
        advance(t);
        return;
    }
    m_overlayLeases[t->token] = paths;
    m_overlayIncoming[t->token] = t;
    auto &c = t->captured;
    if (!main) {
        const auto group = m_options.schema.descriptor(s.basePath);
        if (!group || group->type != ConfigDescriptor::Type::Group) {
            t->error = "overlay-selection";
        } else {
            auto detached = *group;
            for (qsizetype depth = 0; depth < s.basePath.size(); ++depth)
                detached.sensitive |=
                    m_options.schema.descriptor(s.basePath.mid(0, depth))->sensitive;
            c.schema = ConfigSchema(detached);
            for (const auto &relative : c.schema.leaves()) {
                const auto path = s.basePath + relative;
                const auto fence = m_previous.information.value(path);
                c.baseFences[path] = fence;
                c.baseIngress[path] = m_endpoint->revision(path);
                if (fence.latestInput > fence.workerCovered)
                    t->error = "unsettled-overlay-base";
                const auto value = m_previous.values.value(path);
                if (value.isValid())
                    putValue(c.baseValues, relative, value);
            }
        }
    } else {
        const auto descriptor = m_options.schema.descriptor(s.ownerPath);
        const auto state = m_previous.information.value(s.ownerPath);
        c.ownerFence = state;
        c.ownerIngress = m_endpoint->revision(s.ownerPath);
        if (!descriptor ||
            (descriptor->type != ConfigDescriptor::Type::List &&
             descriptor->type != ConfigDescriptor::Type::Record &&
             descriptor->type != ConfigDescriptor::Type::Map) ||
            (s.mode == ConfigOverlayMode::Editor && !m_layers->info(s.ownerPath).route.valid()))
            t->error = "overlay-selection";
        else if (!t->original && s.ownerGeneration != state.publicationGeneration)
            t->error = "overlay-conflict";
        else if (state.latestInput > state.workerCovered)
            t->error = "unsettled-overlay-base";
        else {
            c.ownerValue = m_layers->unmaskedValue(s.ownerPath).value_or(QVariant());
            QList<ConfigRecordListPolicySpec> policies;
            for (const auto &policy : m_listPolicies)
                if (policy.path == s.ownerPath)
                    policies.append(policy);
            c.rawEntryPath =
                overlayRawAddress(c.ownerValue, s.entryPath, policies, m_listMetadata, t->error);
            const auto record = overlayAt(c.ownerValue, c.rawEntryPath);
            const auto recordDescriptor = overlayDescriptor(*descriptor, c.rawEntryPath);
            const auto identityDescriptor =
                recordDescriptor ? overlayDescriptor(*recordDescriptor, s.identityPath)
                                 : std::nullopt;
            const auto identity = record ? overlayAt(*record, s.identityPath) : std::nullopt;
            if (!record || record->metaType().id() != QMetaType::QVariantMap || !recordDescriptor ||
                recordDescriptor->type != ConfigDescriptor::Type::Record || !identityDescriptor ||
                !identity || identityDescriptor->type == ConfigDescriptor::Type::Group ||
                identityDescriptor->type == ConfigDescriptor::Type::Record ||
                identityDescriptor->type == ConfigDescriptor::Type::List ||
                identityDescriptor->type == ConfigDescriptor::Type::Map)
                t->error = "overlay-selection";
            else {
                const auto expected = identityDescriptor->validate(s.identityValue);
                const auto actual = identityDescriptor->validate(*identity);
                if (!expected.ok() || !actual.ok() || actual.value != expected.value)
                    t->error = "overlay-identity";
                const auto map = record->toMap();
                if (map.contains(s.overrideKey) &&
                    map.value(s.overrideKey).metaType().id() != QMetaType::QVariantMap)
                    t->error = "overlay-selection";
                else
                    c.overrides = map.value(s.overrideKey).toMap();
            }
        }
    }
    ++t->acquired;
    if (m_holdOverlayLeases)
        m_overlayPaused[t->token] = t;
    else
        advance(t);
}
void ConfigWorker::holdOverlayLeases(bool hold) {
    m_holdOverlayLeases = hold;
    if (!hold) {
        const auto paused = std::move(m_overlayPaused);
        m_overlayPaused.clear();
        for (const auto &transaction : paused)
            advance(transaction);
    }
}
void ConfigWorker::overlayCommand(const ConfigCommand &command) {
    if (command.operation == "overlayLease") {
        overlayAcquire(command.overlay);
        return;
    }
    auto t = command.overlay;
    if (!t) {
        t = std::make_shared<ConfigOverlayTransaction>();
        t->editorId = command.editorId;
        if (m_overlayCaptures.contains(command.editorId)) {
            t->original = m_overlayCaptures.value(command.editorId);
            t->selection = t->original->selection;
        } else
            t->error = "overlay-owner";
    }
    t->token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t->requestId = command.requestId;
    t->operation = command.operation;
    t->edits = command.overlayEdits;
    t->captured.selection = t->selection;
    if (command.operation == "openOverlay" && t->editorId.isEmpty())
        t->editorId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_overlayTransactions[t->token] = t;
    m_overlayCommands[t->token] = command;
    if (!command.conversionError.isEmpty())
        t->error = command.conversionError;
    if (t->selection.owner && t->selection.base) {
        t->order = {t->selection.owner, t->selection.base};
        if (t->order[1]->order < t->order[0]->order)
            t->order.swapItemsAt(0, 1);
    } else if (t->error.isEmpty())
        t->error = "overlay-owner";
    if (!t->error.isEmpty()) {
        overlayContinue(t);
        return;
    }
    // Register cancellation with the base before the first ordered lease can
    // pause on the owner. The base may close before its acquisition turn.
    if (!t->selection.base->send([t](ConfigWorker &w) {
            w.m_overlayIncoming[t->token] = t;
            if (!w.m_endpoint->live || !t->selection.baseLifetime || !*t->selection.baseLifetime) {
                t->cancelled = true;
                release(t);
            }
        }))
        t->cancelled = true;
    advance(t);
}
void ConfigWorker::overlayContinue(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    try {
        overlayContinueImpl(t);
        m_overlayCommands.remove(t->token);
        m_overlayTransactions.remove(t->token);
    } catch (...) {
        // Keep correlation until the continuation has completed or handed it
        // to persistence. A native predicate must never escape Qt's dispatcher.
        const auto command = m_overlayCommands.take(t->token);
        m_overlayTransactions.remove(t->token);
        t->cancelled = true;
        release(t);
        if (!command.operation.isEmpty()) {
            coverInput(command);
            auto result = resultFor(command);
            result.editorId = t->editorId;
            result.refreshState = "not_refreshed";
            result.status = "failed";
            result.error = {"worker-exception", {}, command.operation};
            if (t->operation == "overlayRefresh") {
                result = m_overlayReceipts.take(t->editorId);
                result.refreshState = "stale";
            }
            fail("worker-exception");
            post(publish(result));
        } else
            fail("worker-exception");
    }
}
void ConfigWorker::overlayContinueImpl(const std::shared_ptr<ConfigOverlayTransaction> &t) {
    if (!m_overlayTransactions.contains(t->token)) {
        release(t);
        return;
    }
    auto command = m_overlayCommands.value(t->token);
    auto result = resultFor(command);
    result.editorId = t->editorId;
    result.refreshState = "not_refreshed";
    if (t->cancelled || !t->selection.owner || !t->selection.owner->live || !t->selection.base ||
        !t->selection.base->live || !t->selection.baseLifetime || !*t->selection.baseLifetime)
        t->error = "overlay-owner";
    const bool apply = t->operation == "overlayApply" || t->operation == "overlayReset";
    const bool refresh = t->operation == "overlayRefresh";
    const bool discard = t->operation == "overlayDiscard";
    if (t->original && !discard && t->selection.mode != ConfigOverlayMode::View &&
        (t->error.isEmpty() || t->error == "overlay-selection" || t->error == "overlay-identity")) {
        const auto &old = *t->original;
        for (auto it = old.baseFences.cbegin(); it != old.baseFences.cend(); ++it)
            if (!sameFence(it.value(), t->captured.baseFences.value(it.key())) ||
                old.baseIngress.value(it.key()) != t->selection.base->revision(it.key()))
                t->error = "overlay-conflict";
        // The apply command itself is one new owner ingress, not an external edit.
        const auto expected = old.ownerIngress + (apply ? 1 : 0);
        if (t->selection.owner->revision(old.selection.ownerPath) != expected ||
            (!refresh && !sameFence(old.ownerFence, t->captured.ownerFence)))
            t->error = "overlay-conflict";
    }
    if (t->original && t->selection.mode == ConfigOverlayMode::View &&
        t->error == "overlay-selection")
        t->error = "overlay-identity";
    auto p = presentation(t->editorId, t->captured);
    if (t->error.isEmpty())
        t->error = p.error;
    if (refresh) {
        result = m_overlayReceipts.take(t->editorId);
        result.refreshState = t->error.isEmpty() ? "current" : "stale";
        if (!t->error.isEmpty()) {
            p = {};
            p.editorId = t->editorId;
            p.conflicted = true;
        } else {
            p.refreshed = true;
            m_overlayCaptures[t->editorId] = t->captured;
        }
        m_overlayPackets.append(p);
        release(t);
        post(publish(result));
        return;
    }
    if (!t->error.isEmpty()) {
        coverInput(command);
        result.status = "failed";
        result.error = {t->error, {}, command.operation};
        if (t->original && t->error != "overlay-busy" && t->error != "overlay-readonly") {
            p = {};
            p.editorId = t->editorId;
            p.conflicted = true;
            if (t->selection.mode == ConfigOverlayMode::View)
                p.error = t->error;
            m_overlayPackets.append(p);
        }
        release(t);
        post(publish(result));
        return;
    }
    if (!apply) {
        p.refreshed = true;
        m_overlayCaptures[t->editorId] = t->captured;
        m_overlayPackets.append(p);
        result.status = discard ? "discarded" : "opened";
        // Worker notifications contain selected revision metadata only.
        ConfigOverlaySubscription subscription;
        subscription.endpoint = m_endpoint;
        subscription.editorId = t->editorId;
        subscription.paths = {t->selection.ownerPath};
        subscription.revisions[t->selection.ownerPath] = t->captured.ownerFence.contentRevision;
        m_overlaySubscriptions[t->editorId + ":owner"] = subscription;
        subscription.paths = t->captured.baseFences.keys();
        subscription.lifetime = t->selection.baseLifetime;
        subscription.revisions.clear();
        for (auto it = t->captured.baseFences.cbegin(); it != t->captured.baseFences.cend(); ++it)
            subscription.revisions[it.key()] = it->contentRevision;
        t->selection.base->send([subscription](ConfigWorker &w) {
            if (subscription.lifetime && *subscription.lifetime && w.m_endpoint->live)
                w.m_overlaySubscriptions[subscription.editorId + ":base"] = subscription;
            else if (auto endpoint = subscription.endpoint.lock())
                endpoint->send([id = subscription.editorId](ConfigWorker &owner) {
                    owner.overlayChanged(id);
                });
        });
        release(t);
        post(publish(command.requestId ? std::optional<ConfigResult>(result) : std::nullopt));
        return;
    }
    auto edited = overlayMerge(t->original->baseValues, t->original->overrides).toMap();
    for (auto it = t->edits.cbegin(); it != t->edits.cend(); ++it)
        if (it.value())
            putValue(edited, it.key(), *it.value());
        else
            removeValue(edited, it.key());
    const auto difference = t->operation == "overlayReset"
                                ? QVariantMap{}
                                : overlayDifference(t->original->schema, t->original->baseValues,
                                                    edited, t->original->overrides);
    auto record = overlayAt(t->captured.ownerValue, t->captured.rawEntryPath)->toMap();
    if (difference.isEmpty())
        record.remove(t->selection.overrideKey);
    else
        record[t->selection.overrideKey] = difference;
    const auto replacement =
        overlayReplace(t->captured.ownerValue, t->captured.rawEntryPath, record);
    auto checked = replacement ? m_options.schema.validate(t->selection.ownerPath, *replacement)
                               : ConfigValueResult{};
    QString error = quarantined(t->selection.ownerPath);
    auto input = *command.input;
    input.edits[t->selection.ownerPath] = checked.value;
    if (!replacement || !checked.ok())
        error =
            checked.errors.isEmpty() ? QString("overlay-selection") : checked.errors.first().code;
    if (error.isEmpty() && m_options.admission) {
        auto leaf = input;
        leaf.kind = ConfigInput::Kind::User;
        leaf.path = t->selection.ownerPath;
        leaf.value = checked.value;
        error = m_options.admission(leaf);
    }
    const auto before = *m_layers;
    if (error.isEmpty()) {
        const auto admitted = m_layers->applyEdits(input.edits);
        if (!admitted.accepted)
            error = admitted.errors.isEmpty() ? QString("admission") : admitted.errors.first().code;
    }
    coverInput(command);
    release(t); // The read leases end after conditional admission, before filesystem publication.
    if (!error.isEmpty()) {
        result.status = "failed";
        result.error = {error, {}, command.operation};
        post(publish(result));
        return;
    }
    acceptIntent(input);
    recordPersistent(input, before);
    command.input = input;
    command.editorId = t->editorId;
    // Advance only the own causal owner base; the frozen plugin base is unchanged.
    t->captured.ownerIngress = m_endpoint->revision(t->selection.ownerPath);
    post(publish());
    t->captured.ownerFence = m_previous.information.value(t->selection.ownerPath);
    m_overlayCaptures[t->editorId] = t->captured;
    m_overlayCommands.remove(t->token);
    m_overlayTransactions.remove(t->token);
    beginSave(command);
}
void ConfigWorker::overlayControl(const std::function<void(ConfigWorker &)> &message) {
    try {
        message(*this);
    } catch (...) {
        fail("worker-exception");
    }
}
void ConfigWorker::overlayScopeClosed(const std::shared_ptr<std::atomic_bool> &lifetime) {
    for (const auto &transaction : m_overlayIncoming)
        if (transaction->selection.baseLifetime == lifetime) {
            transaction->cancelled = true;
            release(transaction);
        }
    for (auto it = m_overlaySubscriptions.begin(); it != m_overlaySubscriptions.end();) {
        if (it->lifetime != lifetime) {
            ++it;
            continue;
        }
        if (auto endpoint = it->endpoint.lock())
            endpoint->send([id = it->editorId](ConfigWorker &w) { w.overlayChanged(id); });
        it = m_overlaySubscriptions.erase(it);
    }
}
void ConfigWorker::overlayNotify() {
    for (auto it = m_overlaySubscriptions.begin(); it != m_overlaySubscriptions.end();) {
        auto endpoint = it->endpoint.lock();
        if (!endpoint || !endpoint->live) {
            it = m_overlaySubscriptions.erase(it);
            continue;
        }
        bool changed = false;
        for (const auto &path : it->paths) {
            const auto revision = m_previous.information.value(path).contentRevision;
            changed |= revision != it->revisions.value(path);
            it->revisions[path] = revision;
        }
        if (changed)
            endpoint->send([id = it->editorId](ConfigWorker &w) { w.overlayChanged(id); });
        ++it;
    }
}
void ConfigWorker::overlayChanged(const QString &id) {
    if (!m_overlayCaptures.contains(id))
        return;
    const auto capture = m_overlayCaptures.value(id);
    if (capture.selection.mode == ConfigOverlayMode::View) {
        ConfigCommand command;
        command.operation = "overlayView";
        command.editorId = id;
        overlayCommand(command);
    } else {
        ConfigOverlayPresentation p;
        p.editorId = id;
        p.conflicted = true;
        m_overlayPackets.append(p);
        post(publish());
    }
}
void ConfigWorker::overlayDetach(const QString &id) {
    auto capture = m_overlayCaptures.take(id);
    m_overlaySubscriptions.remove(id + ":owner");
    if (capture.selection.base)
        capture.selection.base->send(
            [id](ConfigWorker &w) { w.m_overlaySubscriptions.remove(id + ":base"); });
    for (const auto &transaction : m_overlayTransactions)
        if (transaction->editorId == id) {
            transaction->cancelled = true;
            release(transaction);
        }
}
void ConfigWorker::overlayCancelAll() {
    const auto transactions = m_overlayTransactions;
    for (const auto &transaction : transactions) {
        transaction->cancelled = true;
        release(transaction);
    }
    for (const auto &transaction : m_overlayIncoming) {
        transaction->cancelled = true;
        release(transaction);
    }
    m_overlayIncoming.clear();
    m_overlayLeases.clear();
    for (const auto &subscription : m_overlaySubscriptions)
        if (auto endpoint = subscription.endpoint.lock())
            endpoint->send([id = subscription.editorId](ConfigWorker &w) { w.overlayChanged(id); });
}
} // namespace configqml
