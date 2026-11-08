// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"

namespace configqml {
struct ConfigValueSelector {
    enum class Kind { Key, Index, Each, Match };
    Kind kind = Kind::Key;
    QString key;
    qsizetype index = 0;
    QString value;
    bool operator==(const ConfigValueSelector &other) const {
        return kind == other.kind && key == other.key && index == other.index &&
               value == other.value;
    }
};
struct ConfigRecordListPolicySpec {
    QString policyId;
    ConfigPath path;
    QList<ConfigValueSelector> listPath;
    QString keyField;
    QStringList preservedFields;
    bool pruneUnknown = false, normalizeFields = false;
};
struct ConfigRecordListEntry {
    QString key;
    bool available = false, retain = false, normalize = false;
    QVariantMap defaults;
};
struct ConfigRecordListMetadata {
    bool ready = false;
    QList<ConfigRecordListEntry> entries;
};
struct ConfigRecordListEndpoint {
    QString policyId;
    QList<ConfigValueSelector> listAddress;
    quint64 generation = 0;
    // Transfer row index. Native copy/at ignore it; QML forbids that member there.
    qsizetype index = 0;
    bool ensure = false;
};
struct ConfigRecordListEdit {
    enum class Op { Set, Unset, Remove, Insert, Move, Transfer, Copy };
    Op op = Op::Set;
    qsizetype index = 0;
    QString field;
    QVariant value;
    qsizetype to = 0;
    ConfigRecordListEndpoint from, destination;
    std::optional<ConfigRecordListEndpoint> at;
};
struct ConfigRecordListEditOptions {
    quint64 generation = 0;
    QList<ConfigValueSelector> listAddress;
    QList<ConfigRecordListEdit> edits;
    bool ensure = false;
};
// Pure worker-side interpretation; no setting writes, scripts or QObject inputs.
struct ConfigRecordListProjection {
    QVariant value;
    QString error;
    QMap<QList<qsizetype>, QList<qsizetype>> rows;
};
ConfigRecordListProjection projectRecordLists(const ConfigRecordListPolicySpec &policy,
                                              const ConfigRecordListMetadata &metadata,
                                              const QVariant &value);
// Match addresses and typed ensure creation require the schema argument. The
// default preserves the pre-0.4.0 Key/Index-only helper calls.
ConfigValueResult editRecordListValue(const ConfigRecordListPolicySpec &policy,
                                      const ConfigRecordListMetadata &metadata,
                                      const QVariant &value,
                                      const ConfigRecordListEditOptions &options,
                                      const ConfigSchema *schema = nullptr);
// Mutates only detached candidate values; failure leaves both unchanged. Same-leaf
// callers pass the same QVariant reference for source and destination.
QString transferRecordListValue(const ConfigRecordListPolicySpec &fromPolicy,
                                const ConfigRecordListMetadata &fromMetadata, QVariant &fromValue,
                                const ConfigRecordListPolicySpec &toPolicy,
                                const ConfigRecordListMetadata &toMetadata, QVariant &toValue,
                                const ConfigRecordListEdit &edit,
                                const ConfigSchema *schema = nullptr);
QString copyRecordListValue(const ConfigRecordListPolicySpec &fromPolicy,
                            const ConfigRecordListMetadata &fromMetadata, const QVariant &fromValue,
                            const ConfigRecordListPolicySpec &toPolicy,
                            const ConfigRecordListMetadata &toMetadata, QVariant &toValue,
                            const ConfigRecordListEdit &edit, const ConfigSchema &schema);
QString validateRecordListPolicy(const ConfigSchema &schema,
                                 const ConfigRecordListPolicySpec &policy);
} // namespace configqml
