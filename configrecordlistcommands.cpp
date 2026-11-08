// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "configshutdown_p.h"
#include <QSet>

namespace configqml {
quint64 ConfigStore::updateRecordListPolicy(const QString &id,
                                            const ConfigRecordListMetadata &metadata,
                                            QObject *context, Callback callback) {
    const auto requestId = registerRequest(context, std::move(callback));
    ConfigCommand command{requestId, m_sequence, "updateRecordListPolicy", {}};
    command.policyId = ownString(id);
    command.policyMetadata = metadata;
    for (auto &entry : command.policyMetadata->entries) {
        entry.key = ownString(entry.key);
        const auto captured = ownValue(entry.defaults);
        if (!captured.ok())
            command.conversionError = "list-policy-metadata";
        entry.defaults = captured.value.toMap();
    }
    if (!m_accepting || !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = requestId;
        result.storeId = m_storeId;
        result.operation = command.operation;
        result.status = "failed";
        result.error = {"store-closed", {}, command.operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    }
    return requestId;
}
quint64 ConfigStore::editRecordList(const QString &id, const ConfigRecordListEditOptions &options,
                                    QObject *context, Callback callback) {
    const auto requestId = registerRequest(context, std::move(callback));
    ConfigCommand command{requestId, m_sequence + 1, "editRecordList", {}};
    command.policyId = ownString(id);
    command.listEdits = options;
    const auto ownSelectors = [](QList<ConfigValueSelector> &selectors) {
        for (auto &selector : selectors) {
            selector.key = ownString(selector.key);
            selector.value = ownString(selector.value);
        }
    };
    ownSelectors(command.listEdits->listAddress);
    for (auto &edit : command.listEdits->edits) {
        edit.field = ownString(edit.field);
        QList<ConfigRecordListEndpoint *> endpoints;
        if (edit.op == ConfigRecordListEdit::Op::Transfer ||
            edit.op == ConfigRecordListEdit::Op::Copy)
            endpoints = {&edit.from, &edit.destination};
        if (edit.at)
            endpoints.append(&*edit.at);
        for (auto *endpoint : endpoints) {
            endpoint->policyId = ownString(endpoint->policyId);
            ownSelectors(endpoint->listAddress);
        }
        if (edit.op == ConfigRecordListEdit::Op::Set ||
            edit.op == ConfigRecordListEdit::Op::Insert) {
            const auto captured = ownValue(edit.value);
            if (!captured.ok())
                command.conversionError = "list-edit-operation";
            edit.value = captured.value;
        }
    }
    ConfigInput input;
    input.kind = ConfigInput::Kind::Group;
    input.sequence = command.prefix;
    QStringList policies{command.policyId}, written;
    for (const auto &edit : command.listEdits->edits) {
        if (edit.op == ConfigRecordListEdit::Op::Transfer ||
            edit.op == ConfigRecordListEdit::Op::Copy) {
            policies.append({edit.from.policyId, edit.destination.policyId});
            written.append(edit.destination.policyId);
            if (edit.op == ConfigRecordListEdit::Op::Transfer)
                written.append(edit.from.policyId);
        } else {
            const auto target = edit.at ? edit.at->policyId : command.policyId;
            policies.append(target);
            written.append(target);
        }
    }
    if (command.listEdits->edits.isEmpty())
        written.append(command.policyId);
    for (const auto &policyId : policies) {
        if (!m_policyPaths.contains(policyId))
            continue;
        const auto path = m_policyPaths.value(policyId);
        input.fences[path] = info(path);
    }
    for (const auto &policyId : written)
        if (m_policyPaths.contains(policyId)) {
            const auto path = m_policyPaths.value(policyId);
            input.pathGenerations[path] = info(path).pathGeneration + 1;
        }
    command.input = input;
    if (!m_accepting || !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = requestId;
        result.storeId = m_storeId;
        result.operation = command.operation;
        result.status = "failed";
        result.error = {"store-closed", {}, command.operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    } else {
        m_sequence = command.prefix;
        for (auto it = input.pathGenerations.cbegin(); it != input.pathGenerations.cend(); ++it) {
            m_cache.information[it.key()].pathGeneration = it.value();
            m_cache.information[it.key()].latestInput = input.sequence;
        }
    }
    return requestId;
}
void ConfigWorker::recordListCommand(const ConfigCommand &command) {
    auto result = resultFor(command);
    result.policyId = command.policyId;
    auto fail = [&](const QString &code) {
        coverInput(command);
        result.status = "failed";
        result.error = {code, {}, command.operation};
        post(publish(result));
    };
    if (!m_listPolicies.contains(command.policyId)) {
        fail("list-policy-id");
        return;
    }
    const auto policy = m_listPolicies.value(command.policyId);
    if (!command.conversionError.isEmpty()) {
        fail(command.conversionError);
        return;
    }
    if (command.policyMetadata) {
        QSet<QString> keys;
        auto descriptor = *m_options.schema.descriptor(policy.path);
        for (const auto &selector : policy.listPath)
            descriptor = selector.kind == ConfigValueSelector::Kind::Key &&
                                 descriptor.type != ConfigDescriptor::Type::Map
                             ? descriptor.fields.value(selector.key)
                             : descriptor.element.first();
        const auto record = descriptor.element.first();
        for (const auto &entry : command.policyMetadata->entries) {
            if (keys.contains(entry.key)) {
                fail("list-policy-metadata");
                return;
            }
            keys.insert(entry.key);
            for (auto it = entry.defaults.cbegin(); it != entry.defaults.cend(); ++it) {
                const auto field = record.fields.constFind(it.key());
                if (field != record.fields.cend() && !field->validate(it.value()).ok()) {
                    fail("list-policy-metadata");
                    return;
                }
            }
        }
        const auto value = m_layers->unmaskedValue(policy.path);
        if (value && !projectRecordLists(policy, *command.policyMetadata, *value).error.isEmpty()) {
            fail("list-policy-value");
            return;
        }
        m_listMetadata[command.policyId] = *command.policyMetadata;
        result.metadataRevision = ++m_listRevisions[command.policyId];
        result.status = "projected";
        m_acceptOrigin = "projection";
        m_acceptCause = "metadata";
    } else {
        QList<ConfigRecordListEndpoint> endpoints{{command.policyId, command.listEdits->listAddress,
                                                   command.listEdits->generation, 0,
                                                   command.listEdits->ensure}};
        QSet<ConfigPath> transferPaths;
        QSet<ConfigPath> copySources;
        if (command.listEdits->ensure && command.listEdits->edits.isEmpty()) {
            fail("list-edit-operation");
            return;
        }
        for (const auto &edit : command.listEdits->edits) {
            if (edit.op == ConfigRecordListEdit::Op::Transfer ||
                edit.op == ConfigRecordListEdit::Op::Copy) {
                if (edit.from.ensure || edit.at) {
                    fail("list-edit-operation");
                    return;
                }
                for (const auto &endpoint : {edit.from, edit.destination}) {
                    endpoints.append(endpoint);
                    if (m_listPolicies.contains(endpoint.policyId) &&
                        (edit.op == ConfigRecordListEdit::Op::Transfer ||
                         endpoint.policyId == edit.destination.policyId))
                        transferPaths.insert(m_listPolicies.value(endpoint.policyId).path);
                }
                if (edit.op == ConfigRecordListEdit::Op::Copy &&
                    m_listPolicies.contains(edit.from.policyId))
                    copySources.insert(m_listPolicies.value(edit.from.policyId).path);
            } else if (edit.at)
                endpoints.append(*edit.at);
        }
        QMap<ConfigPath, QVariant> values;
        for (const auto &endpoint : endpoints) {
            if (!m_listPolicies.contains(endpoint.policyId)) {
                fail("list-policy-id");
                return;
            }
            const auto path = m_listPolicies.value(endpoint.policyId).path;
            const auto fence = command.input->fences.value(path);
            const auto current = m_previous.information.value(path);
            if (endpoint.generation != current.publicationGeneration ||
                fence.latestInput > fence.workerCovered ||
                fence.pathGeneration != m_pathGenerations.value(path) ||
                fence.contentRevision != current.contentRevision) {
                fail("list-edit-conflict");
                return;
            }
            const auto quarantine = quarantined(path);
            if (!quarantine.isEmpty()) {
                fail(quarantine);
                return;
            }
            if (transferPaths.contains(path)) {
                const auto info = m_layers->info(path);
                if (!info.route.valid() || !info.maskedBy.isEmpty()) {
                    fail(info.route.valid() ? "list-edit-masked" : "no-write-route");
                    return;
                }
            }
            if (copySources.contains(path) && !m_layers->info(path).maskedBy.isEmpty()) {
                fail("list-edit-masked");
                return;
            }
            values[path] = m_layers->unmaskedValue(path).value_or(QVariant());
        }
        QSet<ConfigPath> editedPaths;
        if (command.listEdits->edits.isEmpty())
            editedPaths.insert(policy.path);
        for (const auto &edit : command.listEdits->edits) {
            if (edit.op == ConfigRecordListEdit::Op::Transfer ||
                edit.op == ConfigRecordListEdit::Op::Copy) {
                const auto from = m_listPolicies.value(edit.from.policyId);
                const auto to = m_listPolicies.value(edit.destination.policyId);
                if (edit.op == ConfigRecordListEdit::Op::Transfer)
                    editedPaths.insert(from.path);
                editedPaths.insert(to.path);
                const auto error =
                    edit.op == ConfigRecordListEdit::Op::Transfer
                        ? transferRecordListValue(from, m_listMetadata.value(from.policyId),
                                                  values[from.path], to,
                                                  m_listMetadata.value(to.policyId),
                                                  values[to.path], edit, &m_options.schema)
                        : copyRecordListValue(from, m_listMetadata.value(from.policyId),
                                              values[from.path], to,
                                              m_listMetadata.value(to.policyId), values[to.path],
                                              edit, m_options.schema);
                if (!error.isEmpty()) {
                    fail(error);
                    return;
                }
            } else {
                const auto target = edit.at.value_or(endpoints.first());
                const auto targetPolicy = m_listPolicies.value(target.policyId);
                editedPaths.insert(targetPolicy.path);
                auto options = *command.listEdits;
                options.edits = {edit};
                options.listAddress = target.listAddress;
                options.ensure = target.ensure;
                const auto edited = editRecordListValue(
                    targetPolicy, m_listMetadata.value(target.policyId),
                    values.value(targetPolicy.path), options, &m_options.schema);
                if (!edited.ok()) {
                    fail("list-edit-operation");
                    return;
                }
                values[targetPolicy.path] = edited.value;
            }
        }
        auto input = *command.input;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!editedPaths.contains(it.key()))
                continue;
            const auto checked = m_options.schema.validate(it.key(), it.value());
            if (!checked.ok()) {
                fail(checked.errors.first().code);
                return;
            }
            input.edits[it.key()] = checked.value;
            if (m_options.admission) {
                auto leaf = input;
                leaf.kind = ConfigInput::Kind::User;
                leaf.path = it.key();
                leaf.value = checked.value;
                const auto rejection = m_options.admission(leaf);
                if (!rejection.isEmpty()) {
                    fail(rejection);
                    return;
                }
            }
        }
        const auto before = *m_layers;
        const auto written = m_layers->applyEdits(input.edits);
        if (!written.accepted) {
            fail(written.errors.first().code);
            return;
        }
        coverInput(command);
        acceptIntent(input);
        recordPersistent(input, before);
        scheduleAutoSave();
        result.status = "admitted";
        result.maskedBy = m_layers->info(policy.path).maskedBy;
        for (auto it = input.edits.cbegin(); it != input.edits.cend(); ++it)
            result.coverage.append({it.key(), m_layers->info(it.key()).route.sourceId,
                                    m_pathInputs.value(it.key()), m_pathIntents.value(it.key())});
    }
    if (command.policyMetadata)
        result.coverage.append({policy.path, m_layers->info(policy.path).route.sourceId,
                                m_pathInputs.value(policy.path), m_pathIntents.value(policy.path)});
    post(publish(result));
}
} // namespace configqml
