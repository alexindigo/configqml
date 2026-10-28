// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"

namespace configqml {
struct ConfigModelRow {
    QVariant id;
    QVariant value;
};
struct ConfigModelEdit {
    enum Kind { Insert, Remove, Move, Change } kind;
    int from = 0;
    int to = 0;
    ConfigModelRow row;
    QList<int> roles;
};
struct ConfigModelPlan {
    quint64 base = 0;
    quint64 generation = 0;
    QList<ConfigModelEdit> edits;
    bool reset = false;
    QList<ConfigModelRow> rows;
    QHash<QString, int> rowIndices;
};
QList<ConfigModelRow> projectionRows(const ConfigDescriptor &descriptor, const QVariant &value);
ConfigModelPlan reconcileProjection(const ConfigDescriptor &descriptor,
                                    const QList<ConfigModelRow> &before,
                                    const QList<ConfigModelRow> &after);
QHash<int, QByteArray> projectionRoles(const ConfigDescriptor &descriptor);
QVariant collectionPresentation(const ConfigDescriptor &descriptor, const QVariant &value);
} // namespace configqml
