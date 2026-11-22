"""Generate an ABI probe using only the frozen, installed public API.

The class/type list is discovered once from baseline headers, then the generated
source is retained and compiled unchanged against candidate installed headers.
Public data-member offsets below are explicit: no private-header layout proof.
"""

import re


FIELDS = {
    "ConfigDiagnostic": "path code",
    "ConfigValueResult": "value errors",
    "ConfigDescriptor": "type optional nullable sensitive machineLocal admissionPolicy forbidden defaultValue choices minimum maximum fields element idField stringPattern open",
    "ConfigClosure": "value removed",
    "ConfigMigration": "fromVersion transform",
    "ConfigSource": "id prefix raw bytes revision exists json fallbackFor available diskExists localIntent pruneResetNamespaces unknownData",
    "ConfigLayer": "id kind policy sources writable shippedDefaults",
    "ConfigOperation": "accepted errors",
    "ConfigRoute": "layerId sourceId prefix",
    "ConfigContribution": "layerId sourceId revision sourceExists rawPresent admitted valid maskedBy rawRevision localIntentRevision",
    "ConfigInfo": "declared present layerId sourceId maskedBy route contributions intentRevision",
    "ConfigWriteResult": "accepted errors route maskedBy intentRevision",
    "ConfigEnvironmentMapping": "name path",
    "ConfigEnvironmentResult": "values errors",
    "ConfigEvidenceReference": "evidenceId directory sourceId phase role observationOrder",
    "ConfigError": "code sourceId operation file path line evidence",
    "ConfigBackendCapabilities": "read write watch conditionalPublish affinity",
    "ConfigRevision": "exists token witness",
    "ConfigBackendRequest": "operation sourceId bytes expected",
    "ConfigBackendResult": "requestId sourceId operation success revision bytes publicationState error publicationWitness observedPresence bytesAvailable",
    "ConfigCodecCapabilities": "decode encode preservesUnknownKeys preservesFormatting losslessSigned64 losslessUnsigned64 urls nullable affinity",
    "ConfigDecodeResult": "raw error",
    "ConfigEncodeResult": "bytes error",
    "ConfigJsonEncodeResult": "bytes errors",
    "ConfigJsonLayout": "keys errorOffset",
    "ConfigJsonFile": "layerId sourceId path optional watchMode startupErrorPolicy conflictPolicy deleteWhenEmpty",
    "ConfigFileEdit": "path value ifMissing fencePath pruneEmpty namespaceReset",
    "ConfigSourcePreimage": "revision bytes",
    "ConfigJsonTransaction": "outcome raw published preflightRead candidateBytes preflightBase rebasedOnto overriddenPaths observations warnings",
    "ConfigPathState": "provenance latestInput workerCovered pathGeneration publicationGeneration contentRevision intentRevision",
    "ConfigInput": "kind sequence path value target pathGenerations edits fences",
    "ConfigPublication": "sourceId success revision state error rebasedOnto overriddenPaths",
    "ConfigSourceState": "sourceId prefix path presence load revision quarantined fallbackFor fallbackState diagnostic evidence",
    "ConfigSourceFact": "path present value sourceId revision",
    "ConfigSourceAccepted": "eventId publicationGeneration inputPrefix sources changedPaths origin cause facts",
    "ConfigCoverage": "path sourceId inputThrough intentRevision",
    "ConfigGroupEdit": "path present value",
    "ConfigRetainedGroupOptions": "requestKey paths edits dependencies",
    "ConfigGroupAttempt": "attemptId attemptNumber requestId status publicationState error",
    "ConfigGroupInfo": "groupToken requestKey state capturePrefix coverage dependencies attempts error",
    "ConfigResult": "requestId storeId operation status inputPrefix coveredRevisions publications publicationState error maskedBy masks rejectedPathGenerations evidence policyId metadataRevision coverage groupToken requestKey attemptId capturePrefix attemptNumber dependencies unresolvedGroups excludedGroups editorId refreshState",
    "ConfigRequestHandle": "requestId inputSequence accepted errors",
    "ConfigSnapshot": "values information generation topology inputPrefix",
    "ConfigStoreOptions": "schema layers jsonFiles autoSave autoSaveInterval fileFault storeId modelNumbers projections recordListPolicies evidenceDirectory observedPaths admission profileMaxBytes",
    "ConfigValueSelector": "kind key index value",
    "ConfigRecordListPolicySpec": "policyId path listPath keyField preservedFields pruneUnknown normalizeFields",
    "ConfigRecordListEntry": "key available retain normalize defaults",
    "ConfigRecordListMetadata": "ready entries",
    "ConfigRecordListEndpoint": "policyId listAddress generation index ensure",
    "ConfigRecordListEdit": "op index field value to from destination at",
    "ConfigRecordListEditOptions": "generation listAddress edits ensure",
    "ConfigRecordListProjection": "value error rows",
    "ConfigOverlayOptions": "mode base ownerPath ownerGeneration entryPath identityPath identityValue overrideKey mergePolicy differencePolicy",
    "ConfigProjectionSource": "sourceId layerId prefix presence revision",
    "ConfigProjectionInput": "raw source schemaVersion priorVersion defaults lowerValues values compatibilityRaw metadata",
    "ConfigProjectionEffect": "effectId version action arguments",
    "ConfigProjectionDiagnostic": "code path severity",
    "ConfigProjectionResult": "values compatibilityRaw effects diagnostics",
    "ConfigProjectionSpec": "projectionId sourceIds metadata transform",
    "ConfigProjectionEffectsEvent": "runId projectionId sourceId revision effects",
    "ConfigDeclaration": "sensitive machineLocal admissionPolicy key",
    "ConfigQmlSchema": "version migrations",
    "ConfigQmlValue": "valueType defaultValue choices minimum maximum idField stringPattern optional nullable forbidden open",
    "ConfigMemorySource": "unknownData admissionPolicy layerId sourceId prefix data writable exists kind shippedDefaults",
    "ConfigJsonFileSource": "path optional watchMode fallbackFor startupErrorPolicy conflictPolicy deleteWhenEmpty",
    "ConfigQmlProjection": "projectionId sourceIds metadata transform",
    "ConfigQmlRecordListPolicy": "policyId keyField path preservedFields listPath pruneUnknown normalizeFields",
    "ConfigMemoryBackend::Schedule": "delayed fault",
    "ConfigJsonTransaction::Observation": "result phase role",
}


def generate(headers):
    types, meta = set(), set()
    for header in headers:
        text = header.read_text()
        matches = list(re.finditer(r"^(?:class|struct) (Config\w+)\b[^;{]*\{", text, re.M))
        for index, match in enumerate(matches):
            name = match.group(1)
            types.add(name)
            end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
            if re.search(r"\bQ_(?:OBJECT|GADGET)\b", text[match.end():end]):
                meta.add(name)
    types.update(("ConfigMemoryBackend::Schedule", "ConfigJsonTransaction::Observation"))
    code = ["// Generated from retained v0.5.3 installed public headers."]
    code += [f"#include <{header.name}>" for header in headers]
    code += ["#include <QCoreApplication>", "#include <QJsonDocument>", "#include <QJsonArray>",
             "#include <QJsonObject>", "#include <QMetaMethod>", "#include <QMetaProperty>",
             "#include <QMetaEnum>", "#include <cstdio>", "#include <cstddef>",
             "using namespace configqml;", "int main(int argc, char **argv) {",
             "QCoreApplication app(argc, argv); QJsonObject types, metadata;"]
    for name in sorted(types):
        code.append(f'{{ QJsonObject t{{{{"size", int(sizeof({name}))}}, {{"align", int(alignof({name}))}}}};')
        for field in FIELDS.get(name, "").split():
            code.append(f't["{field}"] = int(offsetof({name}, {field}));')
        code.append(f'types["{name}"] = t; }}')
    for name in sorted(meta):
        code += [f'{{ const auto &m = {name}::staticMetaObject; QJsonObject entry;',
                 'QJsonArray methods, properties, enums;',
                 'for (int i = 0; i < m.methodCount(); ++i) { auto x = m.method(i);',
                 'methods.append(QJsonObject{{"index", i}, {"signature", QString::fromUtf8(x.methodSignature())},',
                 '{"type", int(x.methodType())}, {"access", int(x.access())},',
                 '{"return", QString::fromUtf8(x.typeName())}}); }',
                 'for (int i = 0; i < m.propertyCount(); ++i) { auto p = m.property(i);',
                 'properties.append(QJsonObject{{"index", i}, {"name", QString::fromUtf8(p.name())},',
                 '{"type", QString::fromUtf8(p.typeName())}, {"writable", p.isWritable()},',
                 '{"constant", p.isConstant()}, {"bindable", p.isBindable()},',
                 '{"notify", p.notifySignalIndex()}}); }',
                 'for (int i = 0; i < m.enumeratorCount(); ++i) { auto e = m.enumerator(i); QJsonObject keys;',
                 'for (int k = 0; k < e.keyCount(); ++k) keys[QString::fromUtf8(e.key(k))] = e.value(k);',
                 'enums.append(QJsonObject{{"index", i}, {"name", QString::fromUtf8(e.name())}, {"keys", keys}}); }',
                 'entry["methods"] = methods; entry["properties"] = properties; entry["enums"] = enums;',
                 f'metadata["{name}"] = entry; }}']
    code += ['QJsonObject result{{"layouts", types}, {"metadata", metadata}};',
             'std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData()); }']
    if not types or not meta:
        raise RuntimeError("empty public layout/metadata discovery")
    return "\n".join(code) + "\n"
