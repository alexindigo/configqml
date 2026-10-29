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
    for (auto &selector : command.listEdits->listAddress)
        selector.key = ownString(selector.key);
    for (auto &edit : command.listEdits->edits) {
        edit.field = ownString(edit.field);
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
    const auto path = m_policyPaths.value(id);
    if (m_policyPaths.contains(id)) {
        input.pathGenerations[path] = info(path).pathGeneration + 1;
        input.fences[path] = info(path);
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
        const auto fence = command.input->fences.value(policy.path);
        const auto current = m_previous.information.value(policy.path);
        if (command.listEdits->generation != current.publicationGeneration ||
            fence.latestInput > fence.workerCovered ||
            fence.pathGeneration != m_pathGenerations.value(policy.path) ||
            fence.contentRevision != current.contentRevision) {
            fail("list-edit-conflict");
            return;
        }
        const auto quarantine = quarantined(policy.path);
        if (!quarantine.isEmpty()) {
            fail(quarantine);
            return;
        }
        const auto edited = editRecordListValue(
            policy, m_listMetadata.value(command.policyId),
            m_layers->unmaskedValue(policy.path).value_or(QVariant()), *command.listEdits);
        if (!edited.ok()) {
            fail("list-edit-operation");
            return;
        }
        const auto checked = m_options.schema.validate(policy.path, edited.value);
        if (!checked.ok()) {
            fail(checked.errors.first().code);
            return;
        }
        auto input = *command.input;
        input.edits[policy.path] = checked.value;
        if (m_options.admission) {
            auto leaf = input;
            leaf.kind = ConfigInput::Kind::User;
            leaf.path = policy.path;
            leaf.value = checked.value;
            const auto rejection = m_options.admission(leaf);
            if (!rejection.isEmpty()) {
                fail(rejection);
                return;
            }
        }
        const auto written = m_layers->setUser(policy.path, checked.value);
        if (!written.accepted) {
            fail(written.errors.first().code);
            return;
        }
        coverInput(command);
        acceptIntent(input);
        const auto route = written.route;
        m_dirty[route.sourceId] = input.sequence;
        if (m_files.contains(route.sourceId))
            m_edits[route.sourceId].append(
                {policy.path.mid(route.prefix.size()), checked.value, false});
        scheduleAutoSave();
        result.status = "admitted";
        result.maskedBy = m_layers->info(policy.path).maskedBy;
    }
    result.coverage.append({policy.path, m_layers->info(policy.path).route.sourceId,
                            m_pathInputs.value(policy.path), m_pathIntents.value(policy.path)});
    post(publish(result));
}
} // namespace configqml
