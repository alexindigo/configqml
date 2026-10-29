// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configprojection_p.h"
#include <QAbstractItemModel>
#include <QSet>
#include <algorithm>
#include <vector>

namespace configqml {
QVariant collectionPresentation(const ConfigDescriptor &d, const QVariant &v) {
    using T = ConfigDescriptor::Type;
    if (!v.isValid() || isNull(v))
        return v;
    if (d.type == T::Integer || d.type == T::UnsignedInteger)
        return v.toString();
    if (d.type == T::List) {
        auto list = v.toList();
        for (auto &item : list)
            item = collectionPresentation(d.element.first(), item);
        return list;
    }
    if (d.type == T::Record || d.type == T::Map) {
        auto map = v.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (d.type == T::Map)
                it.value() = collectionPresentation(d.element.first(), it.value());
            else if (d.fields.contains(it.key()))
                it.value() = collectionPresentation(d.fields.value(it.key()), it.value());
        return map;
    }
    return v;
}
QHash<int, QByteArray> projectionRoles(const ConfigDescriptor &d) {
    QHash<int, QByteArray> roles{{Qt::UserRole, "modelData"}, {Qt::UserRole + 1, "rowId"}};
    int role = Qt::UserRole + 2;
    if (d.element.first().type == ConfigDescriptor::Type::Record)
        for (auto it = d.element.first().fields.cbegin(); it != d.element.first().fields.cend();
             ++it)
            roles.insert(role++, it.key().toUtf8());
    return roles;
}
QList<ConfigModelRow> projectionRows(const ConfigDescriptor &d, const QVariant &value) {
    QList<ConfigModelRow> rows;
    if (d.type == ConfigDescriptor::Type::Map) {
        const auto map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            rows.append({it.key(), it.value()});
    } else {
        const auto list = value.toList();
        for (qsizetype i = 0; i < list.size(); ++i)
            rows.append({d.idField.isEmpty() ? QVariant(int(i)) : list[i].toMap().value(d.idField),
                         list[i]});
    }
    return rows;
}
namespace {
// Implicit order-statistic treap: index/rank and relocation are O(log N).
// Stable nodes avoid linear searching or renumbering after each planned move.
struct OrderNode {
    OrderNode *left = nullptr, *right = nullptr, *parent = nullptr;
    quint32 priority = 0;
    int size = 1;
};
int size(OrderNode *n) { return n ? n->size : 0; }
void update(OrderNode *n) {
    if (!n)
        return;
    n->size = 1 + size(n->left) + size(n->right);
    if (n->left)
        n->left->parent = n;
    if (n->right)
        n->right->parent = n;
}
OrderNode *join(OrderNode *a, OrderNode *b) {
    if (!a || !b) {
        auto *n = a ? a : b;
        if (n)
            n->parent = nullptr;
        return n;
    }
    if (a->priority < b->priority) {
        a->right = join(a->right, b);
        update(a);
        a->parent = nullptr;
        return a;
    }
    b->left = join(a, b->left);
    update(b);
    b->parent = nullptr;
    return b;
}
std::pair<OrderNode *, OrderNode *> split(OrderNode *n, int count) {
    if (!n)
        return {nullptr, nullptr};
    if (size(n->left) >= count) {
        auto [a, b] = split(n->left, count);
        n->left = b;
        update(n);
        n->parent = nullptr;
        return {a, n};
    }
    auto [a, b] = split(n->right, count - size(n->left) - 1);
    n->right = a;
    update(n);
    n->parent = nullptr;
    return {n, b};
}
int rank(OrderNode *n) {
    int out = size(n->left);
    while (n->parent) {
        if (n == n->parent->right)
            out += 1 + size(n->parent->left);
        n = n->parent;
    }
    return out;
}
void insert(OrderNode *&root, OrderNode *node, int index) {
    auto [a, b] = split(root, index);
    root = join(join(a, node), b);
}
void remove(OrderNode *&root, int index) {
    auto [a, b] = split(root, index);
    auto [node, c] = split(b, 1);
    root = join(a, c);
    node->left = node->right = node->parent = nullptr;
    node->size = 1;
}
} // namespace
ConfigModelPlan reconcileProjection(const ConfigDescriptor &d, const QList<ConfigModelRow> &before,
                                    const QList<ConfigModelRow> &after) {
    ConfigModelPlan plan;
    plan.rows = after;
    QHash<QString, int> old;
    old.reserve(before.size());
    plan.rowIndices.reserve(after.size());
    for (int i = 0; i < before.size(); ++i)
        old.insert(before[i].id.toString(), i);
    QList<int> sequence, positions, previous(after.size(), -1);
    int added = 0, changed = 0;
    for (int i = 0; i < after.size(); ++i) {
        const auto id = after[i].id.toString();
        plan.rowIndices.insert(id, i);
        const auto found = old.constFind(id);
        if (found == old.cend()) {
            ++added;
            continue;
        }
        changed += before[*found].value != after[i].value;
        sequence.append(*found);
        positions.append(i);
    }
    // Reconstruct one LIS using target positions, in O(N log N).
    QList<int> ends;
    for (int j = 0; j < sequence.size(); ++j) {
        auto it = std::lower_bound(ends.begin(), ends.end(), sequence[j],
                                   [&](int index, int value) { return sequence[index] < value; });
        const int length = int(it - ends.begin());
        previous[positions[j]] = length ? positions[ends[length - 1]] : -1;
        if (it == ends.end())
            ends.append(j);
        else
            *it = j;
    }
    QSet<QString> stationary;
    int cursor = ends.isEmpty() ? -1 : positions[ends.last()];
    while (cursor >= 0) {
        stationary.insert(after[cursor].id.toString());
        cursor = previous[cursor];
    }
    const int removed = int(before.size()) - (int(after.size()) - added);
    const int moves = int(sequence.size()) - int(ends.size());
    plan.reset = added + removed + moves + changed > qMax(64, int(before.size()) / 4);
    if (plan.reset)
        return plan;

    std::vector<OrderNode> nodes(size_t(before.size() + added));
    QHash<QString, OrderNode *> order;
    order.reserve(nodes.size());
    OrderNode *root = nullptr;
    quint32 random = 0x9e3779b9;
    auto create = [&](int i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        nodes[size_t(i)].priority = random;
        return &nodes[size_t(i)];
    };
    for (int i = 0; i < before.size(); ++i) {
        auto *node = create(i);
        order.insert(before[i].id.toString(), node);
        root = join(root, node);
    }
    for (int i = int(before.size()) - 1; i >= 0; --i)
        if (!plan.rowIndices.contains(before[i].id.toString())) {
            plan.edits.append({ConfigModelEdit::Remove, i, i, {}, {}});
            remove(root, i);
        }
    OrderNode *anchor = nullptr;
    int next = int(before.size());
    for (int i = int(after.size()) - 1; i >= 0; --i) {
        const auto id = after[i].id.toString();
        auto *node = order.value(id, nullptr);
        if (!node) {
            node = create(next++);
            const int to = anchor ? rank(anchor) : size(root);
            plan.edits.append({ConfigModelEdit::Insert, to, to, after[i], {}});
            insert(root, node, to);
        } else if (!stationary.contains(id)) {
            const int from = rank(node);
            remove(root, from);
            const int to = anchor ? rank(anchor) : size(root);
            insert(root, node, to);
            if (from != to)
                plan.edits.append({ConfigModelEdit::Move, from, to, {}, {}});
        }
        anchor = node;
    }
    const auto roles = projectionRoles(d);
    for (int i = 0; i < after.size(); ++i) {
        const auto found = old.constFind(after[i].id.toString());
        if (found == old.cend() || before[*found].value == after[i].value)
            continue;
        QList<int> changedRoles{Qt::UserRole};
        if (d.element.first().type == ConfigDescriptor::Type::Record) {
            const auto oldMap = before[*found].value.toMap(), newMap = after[i].value.toMap();
            for (auto it = roles.cbegin(); it != roles.cend(); ++it)
                if (it.key() > Qt::UserRole + 1 && oldMap.value(QString::fromUtf8(it.value())) !=
                                                       newMap.value(QString::fromUtf8(it.value())))
                    changedRoles.append(it.key());
        }
        plan.edits.append({ConfigModelEdit::Change, i, i, after[i], changedRoles});
    }
    return plan;
}
} // namespace configqml
