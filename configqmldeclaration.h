// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configschema.h"
#include "configjsonbackend.h"
#include "configprojection.h"
#include "configrecordlistpolicy.h"
#include <QObject>
#include <QQmlListProperty>
#include <QtQml/qqmlregistration.h>
#include <QJSValue>

namespace configqml {
class ConfigDeclaration : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_CLASSINFO("DefaultProperty", "children")
    Q_PROPERTY(QString key MEMBER key)
    Q_PROPERTY(bool sensitive MEMBER sensitive)
    Q_PROPERTY(bool machineLocal MEMBER machineLocal)
    Q_PROPERTY(AdmissionPolicy admissionPolicy MEMBER admissionPolicy)
    Q_PROPERTY(QQmlListProperty<configqml::ConfigDeclaration> children READ children)
public:
    using QObject::QObject;
    enum AdmissionPolicy { Inherit, Strict, Tolerant };
    Q_ENUM(AdmissionPolicy)
    bool sensitive = false, machineLocal = false;
    AdmissionPolicy admissionPolicy = Inherit;
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
    Q_PROPERTY(int version MEMBER version)
    Q_PROPERTY(QJSValue migrations READ migrationFunctions WRITE setMigrationFunctions)
public:
    using ConfigQmlGroup::ConfigQmlGroup;
    int version = 0;
    QJSValue migrations;
    QJSValue migrationFunctions() const { return migrations; }
    void setMigrationFunctions(const QJSValue &value) { migrations = value; }
    QList<ConfigMigration> compileMigrations(QList<ConfigDiagnostic> &errors) const;
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
    Q_PROPERTY(bool forbidden MEMBER forbidden)
    Q_PROPERTY(QString idField MEMBER idField)
    Q_PROPERTY(QString stringPattern MEMBER stringPattern)
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
        Map,
        SecretRef
    };
    Q_ENUM(ValueType)
    using ConfigDeclaration::ConfigDeclaration;
    ValueType valueType = String;
    QVariant defaultValue;
    QVariantList choices;
    QVariant minimum, maximum;
    QString idField, stringPattern;
    bool optional = false, nullable = false, forbidden = false;
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
    Q_PROPERTY(bool shippedDefaults MEMBER shippedDefaults)
    Q_PROPERTY(AdmissionPolicy admissionPolicy MEMBER admissionPolicy)
public:
    enum Kind { File, Environment, Runtime };
    Q_ENUM(Kind)
    enum AdmissionPolicy { RejectInput, FieldFallback, RetainLastGood };
    Q_ENUM(AdmissionPolicy)
    AdmissionPolicy admissionPolicy = FieldFallback;
    using QObject::QObject;
    QString layerId = "user", sourceId = "memory";
    QStringList prefix;
    QVariantMap data;
    bool writable = true, exists = true;
    Kind kind = File;
    bool shippedDefaults = false;
};
class ConfigJsonFileSource : public ConfigMemorySource {
    Q_OBJECT
    QML_NAMED_ELEMENT(JsonFileSource)
    Q_PROPERTY(QString path MEMBER path)
    Q_PROPERTY(bool optional MEMBER optional)
    Q_PROPERTY(WatchMode watchMode MEMBER watchMode)
    Q_PROPERTY(QString fallbackFor MEMBER fallbackFor)
    Q_PROPERTY(StartupErrorPolicy startupErrorPolicy MEMBER startupErrorPolicy)
    Q_PROPERTY(ConflictPolicy conflictPolicy MEMBER conflictPolicy)
    Q_PROPERTY(bool deleteWhenEmpty MEMBER deleteWhenEmpty)
public:
    enum WatchMode { Off, StartupSnapshot, Live };
    Q_ENUM(WatchMode)
    enum StartupErrorPolicy { FailReady, SkipProjectionRetainError };
    Q_ENUM(StartupErrorPolicy)
    enum ConflictPolicy { PathIntent, StrictSourcePreimage };
    Q_ENUM(ConflictPolicy)
    explicit ConfigJsonFileSource(QObject *parent = nullptr) : ConfigMemorySource(parent) {
        sourceId = "json";
    }
    QString path;
    bool optional = true;
    WatchMode watchMode = StartupSnapshot;
    QString fallbackFor;
    StartupErrorPolicy startupErrorPolicy = FailReady;
    ConflictPolicy conflictPolicy = PathIntent;
    bool deleteWhenEmpty = false;
};
class ConfigQmlProjection : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigProjection)
    Q_PROPERTY(QString projectionId MEMBER projectionId)
    Q_PROPERTY(QStringList sourceIds MEMBER sourceIds)
    Q_PROPERTY(QJSValue metadata READ metadataValue WRITE setMetadata)
    Q_PROPERTY(QJSValue transform READ transformValue WRITE setTransform)
public:
    using QObject::QObject;
    QString projectionId;
    QStringList sourceIds;
    QJSValue metadata, transform;
    QJSValue metadataValue() const { return metadata; }
    void setMetadata(const QJSValue &value) { metadata = value; }
    QJSValue transformValue() const { return transform; }
    void setTransform(const QJSValue &value) { transform = value; }
    ConfigProjectionSpec compile(QList<ConfigDiagnostic> &errors) const;
};
class ConfigQmlRecordListPolicy : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ConfigRecordListPolicy)
    Q_PROPERTY(QString policyId MEMBER policyId)
    Q_PROPERTY(QStringList path MEMBER path)
    Q_PROPERTY(QJSValue listPath READ listPathValue WRITE setListPath)
    Q_PROPERTY(QString keyField MEMBER keyField)
    Q_PROPERTY(QStringList preservedFields MEMBER preservedFields)
    Q_PROPERTY(bool pruneUnknown MEMBER pruneUnknown)
    Q_PROPERTY(bool normalizeFields MEMBER normalizeFields)
public:
    using QObject::QObject;
    QString policyId, keyField;
    QStringList path, preservedFields;
    QJSValue listPath;
    QJSValue listPathValue() const { return listPath; }
    void setListPath(const QJSValue &value) { listPath = value; }
    bool pruneUnknown = false, normalizeFields = false;
    ConfigRecordListPolicySpec compile(QList<ConfigDiagnostic> &errors) const;
};
} // namespace configqml
