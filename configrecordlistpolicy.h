// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"

namespace configqml {
struct ConfigValueSelector {
    enum class Kind { Key, Index, Each };
    Kind kind = Kind::Key;
    QString key;
    qsizetype index = 0;
    bool operator==(const ConfigValueSelector &other) const {
        return kind == other.kind && key == other.key && index == other.index;
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
struct ConfigRecordListEdit {
    enum class Op { Set, Unset, Remove, Insert, Move };
    Op op = Op::Set;
    qsizetype index = 0;
    QString field;
    QVariant value;
    qsizetype to = 0;
};
struct ConfigRecordListEditOptions {
    quint64 generation = 0;
    QList<ConfigValueSelector> listAddress;
    QList<ConfigRecordListEdit> edits;
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
ConfigValueResult editRecordListValue(const ConfigRecordListPolicySpec &policy,
                                      const ConfigRecordListMetadata &metadata,
                                      const QVariant &value,
                                      const ConfigRecordListEditOptions &options);
QString validateRecordListPolicy(const ConfigSchema &schema,
                                 const ConfigRecordListPolicySpec &policy);
} // namespace configqml
