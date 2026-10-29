// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configqmlvalue.h"
#include "configconsumer_p.h"
#include <cmath>

namespace configqml {
namespace {
bool exact(const QVariantMap &map, QStringList required, const QStringList &optional = {}) {
    for (const auto &key : map.keys())
        if (!required.contains(key) && !optional.contains(key))
            return false;
    for (const auto &key : required)
        if (!map.contains(key))
            return false;
    return true;
}
bool string(const QVariant &value) { return value.metaType().id() == QMetaType::QString; }
bool boolean(const QVariant &value) { return value.metaType().id() == QMetaType::Bool; }
bool map(const QVariant &value) { return value.metaType().id() == QMetaType::QVariantMap; }
bool list(const QVariant &value) { return value.metaType().id() == QMetaType::QVariantList; }
bool index(const QVariant &value, qsizetype &out) { return qmlIndex(value, out); }
bool selectors(const QVariant &value, QList<ConfigValueSelector> &out, bool each) {
    return qmlSelectors(value, out, each);
}
QVariantMap errorMap(const ConfigError &error) {
    return {{"code", error.code}, {"sourceId", error.sourceId}, {"file", error.file},
            {"path", error.path}, {"line", error.line},         {"operation", error.operation}};
}
QVariantList evidenceList(const QList<ConfigEvidenceReference> &references) {
    QVariantList result;
    for (const auto &ref : references)
        result.append(QVariantMap{{"evidenceId", ref.evidenceId},
                                  {"directory", ref.directory},
                                  {"sourceId", ref.sourceId},
                                  {"phase", ref.phase},
                                  {"role", ref.role},
                                  {"observationOrder", QString::number(ref.observationOrder)}});
    return result;
}
} // namespace
ConfigRecordListPolicySpec
ConfigQmlRecordListPolicy::compile(QList<ConfigDiagnostic> &errors) const {
    ConfigRecordListPolicySpec spec;
    spec.policyId = ownString(policyId);
    spec.path = ownPath(path);
    spec.keyField = ownString(keyField);
    for (const auto &field : preservedFields)
        spec.preservedFields.append(ownString(field));
    spec.pruneUnknown = pruneUnknown;
    spec.normalizeFields = normalizeFields;
    if (!listPath.isUndefined()) {
        const auto captured = captureQmlValue(QVariant::fromValue(listPath));
        if (!captured.ok() || !selectors(captured.value, spec.listPath, true))
            errors.append(ConfigDiagnosticBuilder().make({}, "list-policy-declaration"));
    }
    return spec;
}
QVariantList ConfigQmlStore::sourceStates() const {
    QVariantList result;
    if (!m_core)
        return result;
    for (const auto &state : m_core->sourceStates())
        result.append(
            QVariantMap{{"sourceId", state.sourceId},
                        {"prefix", state.prefix},
                        {"path", state.path},
                        {"presence", state.presence},
                        {"load", state.load},
                        {"revision", QString::fromLatin1(state.revision.token)},
                        {"quarantined", state.quarantined},
                        {"fallbackFor", state.fallbackFor},
                        {"fallbackState", state.fallbackState},
                        {"diagnostic", errorMap(m_diagnostics.sanitize(state.diagnostic))},
                        {"evidence", evidenceList(state.evidence)}});
    return result;
}
void ConfigQmlStore::initializeCompatibilitySignals() {
    connect(m_core, &ConfigStore::sourceStatesChanged, this,
            [this] { ConfigConsumerGuard(this).invoke([&] { emit sourceStatesChanged(); }); });
    connect(m_core, &ConfigStore::sourceAccepted, this, [this](const ConfigSourceAccepted &event) {
        QVariantList sources, paths, facts;
        for (const auto &source : event.sources)
            sources.append(QVariantMap{{"sourceId", source.first},
                                       {"revision", QString::fromLatin1(source.second.token)}});
        for (const auto &path : event.changedPaths)
            paths.append(QVariant(path));
        const auto schema = m_core->schema();
        for (const auto &fact : event.facts) {
            QVariantMap record{{"path", fact.path},
                               {"present", fact.present},
                               {"sourceId", fact.sourceId},
                               {"revision", QString::fromLatin1(fact.revision.token)}};
            if (fact.present)
                record["value"] = qmlValue(*schema.descriptor(fact.path), fact.value);
            facts.append(record);
        }
        const QVariantMap value{
            {"eventId", event.eventId},
            {"publicationGeneration", QString::number(event.publicationGeneration)},
            {"inputPrefix", QString::number(event.inputPrefix)},
            {"sources", sources},
            {"changedPaths", paths},
            {"origin", event.origin},
            {"cause", event.cause},
            {"facts", facts}};
        ConfigConsumerGuard(this).invoke([&] { emit sourceAccepted(value); });
    });
    connect(m_core, &ConfigStore::projectionEffects, this,
            [this](const ConfigProjectionEffectsEvent &event) {
                QVariantList effects;
                for (const auto &effect : event.effects)
                    effects.append(QVariantMap{{"effectId", effect.effectId},
                                               {"version", double(effect.version)},
                                               {"action", effect.action},
                                               {"arguments", effect.arguments}});
                const QVariantMap value{{"runId", event.runId},
                                        {"projectionId", event.projectionId},
                                        {"sourceId", event.sourceId},
                                        {"revision", event.revision},
                                        {"effects", effects}};
                ConfigConsumerGuard(this).invoke([&] { emit projectionEffects(value); });
            });
}
QString ConfigQmlStore::updateRecordListPolicy(const QString &id, const QJSValue &metadata,
                                               const QJSValue &function) {
    ConfigConsumerGuard boundary(this);
    const auto core = m_core;
    ConfigValueResult captured;
    if (!boundary.invoke([&] { captured = captureQmlValue(QVariant::fromValue(metadata)); }))
        return {};
    if (!m_core || core != m_core || m_draft || !m_writable) {
        reject({}, "not-ready");
        return {};
    }
    const auto record = captured.value.toMap();
    bool valid = captured.ok() && map(captured.value) && exact(record, {"ready", "entries"}) &&
                 boolean(record.value("ready")) && list(record.value("entries"));
    ConfigRecordListMetadata input;
    input.ready = record.value("ready").toBool();
    for (const auto &raw : record.value("entries").toList()) {
        const auto entry = raw.toMap();
        valid &= map(raw) &&
                 exact(entry, {"key", "available", "retain", "normalize", "defaults"}) &&
                 string(entry.value("key")) && boolean(entry.value("available")) &&
                 boolean(entry.value("retain")) && boolean(entry.value("normalize")) &&
                 map(entry.value("defaults"));
        input.entries.append({entry.value("key").toString(), entry.value("available").toBool(),
                              entry.value("retain").toBool(), entry.value("normalize").toBool(),
                              entry.value("defaults").toMap()});
    }
    if (!valid) {
        reject({}, captured.ok() ? "list-policy-metadata" : captured.errors.first().code);
        return {};
    }
    if (m_policyRecords.contains(id))
        for (auto &entry : input.entries)
            for (auto it = entry.defaults.begin(); it != entry.defaults.end(); ++it) {
                const auto recordDescriptor = m_policyRecords.value(id);
                const auto field = recordDescriptor.fields.constFind(it.key());
                if (field == recordDescriptor.fields.cend())
                    continue;
                const auto checked = admitQmlValue(*field, it.value());
                if (!checked.ok()) {
                    reject({}, checked.errors.first().code);
                    return {};
                }
                it.value() = checked.value;
            }
    quint64 requestId = 0;
    boundary.invoke(
        [&] { requestId = core->updateRecordListPolicy(id, input, this, callback(function)); });
    return QString::number(requestId);
}
QString ConfigQmlStore::editRecordList(const QString &id, const QJSValue &options,
                                       const QJSValue &function) {
    ConfigConsumerGuard boundary(this);
    const auto core = m_core;
    ConfigValueResult captured;
    if (!boundary.invoke([&] { captured = captureQmlValue(QVariant::fromValue(options)); }))
        return {};
    if (!m_core || core != m_core || m_draft || !m_writable) {
        reject({}, "not-ready");
        return {};
    }
    const auto record = captured.value.toMap();
    ConfigRecordListEditOptions input;
    bool generationValid = false;
    const auto generation = record.value("generation");
    input.generation = generation.toString().toULongLong(&generationValid);
    bool valid = captured.ok() && map(captured.value) &&
                 exact(record, {"generation", "listAddress", "edits"}) && string(generation) &&
                 generationValid && QString::number(input.generation) == generation.toString() &&
                 selectors(record.value("listAddress"), input.listAddress, false) &&
                 list(record.value("edits"));
    for (const auto &raw : record.value("edits").toList()) {
        const auto edit = raw.toMap();
        const auto op = edit.value("op").toString();
        ConfigRecordListEdit command;
        valid &= map(raw) && string(edit.value("op")) && index(edit.value("index"), command.index);
        if (op == "set" || op == "unset") {
            command.op =
                op == "set" ? ConfigRecordListEdit::Op::Set : ConfigRecordListEdit::Op::Unset;
            valid &= exact(edit, op == "set" ? QStringList{"op", "index", "field", "value"}
                                             : QStringList{"op", "index", "field"}) &&
                     string(edit.value("field"));
            command.field = edit.value("field").toString();
            command.value = edit.value("value");
        } else if (op == "remove") {
            command.op = ConfigRecordListEdit::Op::Remove;
            valid &= exact(edit, {"op", "index"});
        } else if (op == "insert") {
            command.op = ConfigRecordListEdit::Op::Insert;
            valid &= exact(edit, {"op", "index", "value"});
            command.value = edit.value("value");
        } else if (op == "move") {
            command.op = ConfigRecordListEdit::Op::Move;
            valid &= exact(edit, {"op", "index", "to"}) && index(edit.value("to"), command.to);
        } else
            valid = false;
        input.edits.append(command);
    }
    if (!valid) {
        reject({}, captured.ok() ? "list-edit-operation" : captured.errors.first().code);
        return {};
    }
    if (m_policyRecords.contains(id)) {
        const auto recordDescriptor = m_policyRecords.value(id);
        for (auto &edit : input.edits) {
            if (edit.op == ConfigRecordListEdit::Op::Insert) {
                const auto checked = admitQmlValue(recordDescriptor, edit.value);
                if (!checked.ok()) {
                    reject({}, checked.errors.first().code);
                    return {};
                }
                edit.value = checked.value;
            } else if (edit.op == ConfigRecordListEdit::Op::Set) {
                const auto field = recordDescriptor.fields.constFind(edit.field);
                if (field == recordDescriptor.fields.cend())
                    continue;
                const auto checked = admitQmlValue(*field, edit.value);
                if (!checked.ok()) {
                    reject({}, checked.errors.first().code);
                    return {};
                }
                edit.value = checked.value;
            }
        }
    }
    quint64 requestId = 0;
    boundary.invoke([&] { requestId = core->editRecordList(id, input, this, callback(function)); });
    return QString::number(requestId);
}
} // namespace configqml
