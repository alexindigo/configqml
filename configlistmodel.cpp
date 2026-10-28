// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlistmodel.h"
#include "configprojection_p.h"
#include "configconsumer_p.h"
#include <QThread>
#include <QElapsedTimer>

namespace configqml {
ConfigNode ConfigNode::fromValue(const ConfigSchema &schema, const ConfigPath &path,
                                 const QVariant &value, bool qmlNumbers) {
    const auto descriptor = schema.descriptor(path);
    const auto captured = schema.validate(path, value);
    return descriptor && captured.ok() ? ConfigNode(*descriptor, captured.value, qmlNumbers)
                                       : ConfigNode();
}
ConfigNode::ConfigNode(ConfigDescriptor d, QVariant v, bool qml)
    : m_descriptor(std::move(d)), m_value(std::move(v)), m_qmlNumbers(qml) {}
QVariant ConfigNode::value() const {
    if (m_value.metaType().id() == QMetaType::QVariantList ||
        m_value.metaType().id() == QMetaType::QVariantMap)
        return {};
    if (m_qmlNumbers &&
        (m_descriptor.type == ConfigDescriptor::Type::Integer ||
         m_descriptor.type == ConfigDescriptor::Type::UnsignedInteger) &&
        m_value.isValid() && !isNull(m_value))
        return m_value.toString();
    return m_value;
}
int ConfigNode::count() const {
    return m_value.metaType().id() == QMetaType::QVariantMap ? m_value.toMap().size()
                                                             : m_value.toList().size();
}
ConfigNode ConfigNode::get(const QString &key) const {
    const auto map = m_value.toMap();
    if (!map.contains(key))
        return {};
    auto d = m_descriptor.type == ConfigDescriptor::Type::Map && !m_descriptor.element.isEmpty()
                 ? m_descriptor.element.first()
                 : m_descriptor.fields.value(key);
    return ConfigNode(d, map.value(key), m_qmlNumbers);
}
ConfigNode ConfigNode::at(int index) const {
    const auto list = m_value.toList();
    if (index < 0 || index >= list.size())
        return {};
    return ConfigNode(m_descriptor.element.value(0), list[index], m_qmlNumbers);
}
struct ConfigListModel::Rows {
    QList<ConfigModelRow> values;
};
ConfigListModel::ConfigListModel(ConfigDescriptor d, Numbers numbers)
    : m_descriptor(std::move(d)), m_roles(projectionRoles(m_descriptor)),
      m_rows(std::make_unique<Rows>()), m_numbers(numbers) {}
ConfigListModel::~ConfigListModel() = default;
int ConfigListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_rows->values.size());
}
QVariant ConfigListModel::data(const QModelIndex &index, int role) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!index.isValid() || index.model() != this || index.column() != 0 || index.row() < 0 ||
        index.row() >= m_rows->values.size())
        return {};
    const auto &row = m_rows->values[index.row()];
    auto scalar = [&](const ConfigDescriptor &d, const QVariant &value) -> QVariant {
        if (m_numbers == Numbers::DecimalStrings && value.isValid() && !isNull(value) &&
            (d.type == ConfigDescriptor::Type::Integer ||
             d.type == ConfigDescriptor::Type::UnsignedInteger))
            return value.toString();
        return value;
    };
    if (role == Qt::UserRole + 1)
        return m_descriptor.idField.isEmpty()
                   ? row.id
                   : scalar(m_descriptor.element.first().fields.value(m_descriptor.idField),
                            row.id);
    if (role == Qt::UserRole) {
        if (row.value.metaType().id() != QMetaType::QVariantMap &&
            row.value.metaType().id() != QMetaType::QVariantList)
            return scalar(m_descriptor.element.first(), row.value);
        return QVariant::fromValue(ConfigNode(m_descriptor.element.first(), row.value,
                                              m_numbers == Numbers::DecimalStrings));
    }
    if (!m_roles.contains(role))
        return {};
    const auto key = QString::fromUtf8(m_roles.value(role));
    const auto value = row.value.toMap().value(key);
    if (value.metaType().id() == QMetaType::QVariantMap ||
        value.metaType().id() == QMetaType::QVariantList) {
        return QVariant::fromValue(ConfigNode(m_descriptor.element.first().fields.value(key), value,
                                              m_numbers == Numbers::DecimalStrings));
    }
    return scalar(m_descriptor.element.first().fields.value(key), value);
}
Qt::ItemFlags ConfigListModel::flags(const QModelIndex &index) const {
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}
int ConfigListModel::indexOfRow(const QVariant &id) const {
    if (!id.isValid() || isNull(id))
        return -1;
    if (m_applying) {
        for (int i = 0; i < m_rows->values.size(); ++i)
            if (m_rows->values[i].id.toString() == id.toString())
                return i;
        return -1;
    }
    return m_rowIndices.value(id.toString(), -1);
}
bool ConfigListModel::setData(const QModelIndex &, const QVariant &, int) {
    ConfigConsumerGuard(this).invoke([&] { emit writeRejected(); });
    return false;
}
bool ConfigListModel::apply(const ConfigModelPlan &plan, QObject *owner) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(plan.base == m_generation);
    ConfigConsumerGuard guard(this);
    QPointer<QObject> ownerGuard(owner);
    QElapsedTimer timer;
    timer.start();
    qint64 notifications = 0;
    auto notify = [&](auto function) {
        QElapsedTimer elapsed;
        elapsed.start();
        const bool alive = guard.invoke(function);
        notifications += elapsed.nsecsElapsed();
        return alive;
    };
    m_applying = true;
    if (plan.reset) {
        if (!notify([&] { beginResetModel(); }))
            return false;
        m_rows->values = plan.rows;
        m_rowIndices = plan.rowIndices;
        m_applying = false;
        if (!notify([&] { endResetModel(); }))
            return false;
        if (!ownerGuard)
            return false;
    }
    for (const auto &edit : plan.edits) {
        if (edit.kind == ConfigModelEdit::Insert) {
            if (!notify([&] { beginInsertRows({}, edit.from, edit.from); }))
                return false;
            m_rows->values.insert(edit.from, edit.row);
            if (!notify([&] { endInsertRows(); }))
                return false;
        } else if (edit.kind == ConfigModelEdit::Remove) {
            if (!notify([&] { beginRemoveRows({}, edit.from, edit.from); }))
                return false;
            m_rows->values.removeAt(edit.from);
            if (!notify([&] { endRemoveRows(); }))
                return false;
        } else if (edit.kind == ConfigModelEdit::Move) {
            bool started = false;
            if (!notify([&] {
                    started = beginMoveRows({}, edit.from, edit.from, {},
                                            edit.to > edit.from ? edit.to + 1 : edit.to);
                }))
                return false;
            Q_ASSERT(started);
            m_rows->values.move(edit.from, edit.to);
            if (!notify([&] { endMoveRows(); }))
                return false;
        } else {
            m_rows->values[edit.from] = edit.row;
            if (!notify([&] { emit dataChanged(index(edit.from), index(edit.from), edit.roles); }))
                return false;
        }
        if (!ownerGuard)
            return false;
    }
    m_rowIndices = plan.rowIndices;
    m_applying = false;
    m_generation = plan.generation;
    const bool alive = notify([&] { emit generationChanged(); });
    if (alive) {
        m_lastLibraryNanos = timer.nsecsElapsed() - notifications;
        m_lastNotificationNanos = notifications;
    }
    return alive;
}
} // namespace configqml
