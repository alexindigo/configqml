// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include <QAbstractListModel>
#include <QPointer>
#include <memory>

namespace configqml {
struct ConfigModelPlan;
class ConfigStore;
// Immutable owned value snapshot. Composite values are accessible only through child nodes.
class ConfigNode {
    Q_GADGET
    Q_PROPERTY(QVariant value READ value CONSTANT)
    Q_PROPERTY(QStringList keys READ keys CONSTANT)
    Q_PROPERTY(int count READ count CONSTANT)
    Q_PROPERTY(bool valid READ valid CONSTANT)
public:
    ConfigNode() = default;
    static ConfigNode fromValue(const ConfigSchema &schema, const ConfigPath &path,
                                const QVariant &value, bool qmlNumbers = false);
    QVariant value() const;
    QStringList keys() const { return m_value.toMap().keys(); }
    int count() const;
    bool valid() const { return m_value.isValid(); }
    Q_INVOKABLE configqml::ConfigNode get(const QString &key) const;
    Q_INVOKABLE configqml::ConfigNode at(int index) const;

private:
    friend class ConfigListModel;
    // Only producers of already-owned, admitted snapshots may use this path.
    ConfigNode(ConfigDescriptor descriptor, QVariant value, bool qmlNumbers = false);
    ConfigDescriptor m_descriptor;
    QVariant m_value;
    bool m_qmlNumbers = false;
};
class ConfigListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString generation READ generation NOTIFY generationChanged)
public:
    enum class Numbers { Native, DecimalStrings };
    ~ConfigListModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return m_roles; }
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &, const QVariant &, int) override;
    QString generation() const { return QString::number(m_generation); }
    Q_INVOKABLE int indexOfRow(const QVariant &rowId) const;
    ConfigDescriptor descriptor() const { return m_descriptor; }
signals:
    void generationChanged();
    void writeRejected();

private:
    friend class ConfigStore;
    friend class ConfigStoreTestAccess;
    ConfigListModel(ConfigDescriptor descriptor, Numbers numbers);
    bool apply(const ConfigModelPlan &plan, QObject *owner);
    ConfigDescriptor m_descriptor;
    QHash<int, QByteArray> m_roles;
    struct Rows;
    std::unique_ptr<Rows> m_rows;
    quint64 m_generation = 0;
    Numbers m_numbers;
    QHash<QString, int> m_rowIndices;
    bool m_applying = false;
    qint64 m_lastLibraryNanos = 0;
    qint64 m_lastNotificationNanos = 0;
};
} // namespace configqml
