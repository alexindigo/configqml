// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include <QObject>
#include <QQmlListProperty>
#include <QtQml/qqmlregistration.h>

namespace configqml {
class ConfigDeclaration : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_CLASSINFO("DefaultProperty", "children")
    Q_PROPERTY(QString key MEMBER key)
    Q_PROPERTY(QQmlListProperty<configqml::ConfigDeclaration> children READ children)
public:
    using QObject::QObject;
    QString key;
    QQmlListProperty<ConfigDeclaration> children() { return {this, &m_children}; }
    virtual ConfigDescriptor compile(QList<ConfigDiagnostic> &errors,
                                     const ConfigPath &path = {}) const = 0;

protected:
    QList<ConfigDeclaration *> m_children;
};
class ConfigQmlGroup : public ConfigDeclaration {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigGroup)
public:
    using ConfigDeclaration::ConfigDeclaration;
    ConfigDescriptor compile(QList<ConfigDiagnostic> &errors,
                             const ConfigPath &path = {}) const override;
};
class ConfigQmlSchema : public ConfigQmlGroup {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigSchema)
public:
    using ConfigQmlGroup::ConfigQmlGroup;
};
class ConfigQmlValue : public ConfigDeclaration {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigValue)
    Q_PROPERTY(ValueType valueType MEMBER valueType)
    Q_PROPERTY(QVariant defaultValue MEMBER defaultValue)
    Q_PROPERTY(QVariantList choices MEMBER choices)
    Q_PROPERTY(QVariant minimum MEMBER minimum)
    Q_PROPERTY(QVariant maximum MEMBER maximum)
    Q_PROPERTY(bool optional MEMBER optional)
    Q_PROPERTY(bool nullable MEMBER nullable)
    Q_PROPERTY(bool sensitive MEMBER sensitive)
    Q_PROPERTY(bool forbidden MEMBER forbidden)
public:
    enum ValueType {
        Boolean,
        Integer,
        Int64,
        UInt64,
        Real,
        String,
        Url,
        List,
        StringList,
        Record,
        Map
    };
    Q_ENUM(ValueType)
    using ConfigDeclaration::ConfigDeclaration;
    ValueType valueType = String;
    QVariant defaultValue;
    QVariantList choices;
    QVariant minimum, maximum;
    bool optional = false, nullable = false, sensitive = false, forbidden = false;
    ConfigDescriptor compile(QList<ConfigDiagnostic> &errors,
                             const ConfigPath &path = {}) const override;
};
class ConfigMemorySource : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(MemorySource)
    Q_PROPERTY(QString layerId MEMBER layerId)
    Q_PROPERTY(QString sourceId MEMBER sourceId)
    Q_PROPERTY(QStringList prefix MEMBER prefix)
    Q_PROPERTY(QVariantMap data MEMBER data)
    Q_PROPERTY(bool writable MEMBER writable)
    Q_PROPERTY(bool exists MEMBER exists)
    Q_PROPERTY(Kind kind MEMBER kind)
public:
    enum Kind { File, Environment, Runtime };
    Q_ENUM(Kind)
    using QObject::QObject;
    QString layerId = "user", sourceId = "memory";
    QStringList prefix;
    QVariantMap data;
    bool writable = true, exists = true;
    Kind kind = File;
};
} // namespace configqml
