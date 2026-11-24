// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configfacade.h"
#include "confignumber.h"
#include "configqmloverlay.h"
#include "configstorehandle.h"
#include <QFile>
#include <QMetaEnum>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>
#include <array>
#include <memory>

using namespace configqml;

namespace {
template <class... Types> constexpr auto metaObjects() {
    return std::array{&Types::staticMetaObject...};
}

QStringList legacyScopeMembers() {
    // Include base classes and runtime-only returned values too. The latter
    // are not creatable with QML syntax but still expose their metaobjects.
    const auto meta =
        metaObjects<ConfigDeclaration, ConfigQmlGroup, ConfigQmlSchema, ConfigQmlValue,
                    ConfigMemorySource, ConfigJsonFileSource, ConfigQmlProjection,
                    ConfigQmlRecordListPolicy, ConfigQmlStore, ConfigQmlOverlay, ConfigFacade,
                    ConfigStore, ConfigStoreHandle, ConfigDraft, ConfigOverlayEditor,
                    ConfigListModel, ConfigNode, ConfigNumber>();
    QStringList members;
    for (const auto *object : meta) {
        const auto prefix = QString::fromLatin1(object->className()) + ' ';
        for (int i = object->propertyOffset(); i < object->propertyCount(); ++i)
            members << prefix + "property " + QString::fromLatin1(object->property(i).name());
        for (int i = object->methodOffset(); i < object->methodCount(); ++i) {
            const auto method = object->method(i);
            if (method.methodType() == QMetaMethod::Signal)
                members << prefix + "signal " + QString::fromLatin1(method.methodSignature());
            else if (method.access() == QMetaMethod::Public)
                members << prefix + "method " + QString::fromLatin1(method.methodSignature());
        }
        for (int i = object->enumeratorOffset(); i < object->enumeratorCount(); ++i) {
            const auto enumeration = object->enumerator(i);
            for (int key = 0; key < enumeration.keyCount(); ++key)
                members << prefix + "enum " + QString::fromLatin1(enumeration.name()) +
                               "::" + QString::fromLatin1(enumeration.key(key));
        }
    }
    members.sort();
    return members;
}
} // namespace

class QmlLegacyScopeTest : public QObject {
    Q_OBJECT
private slots:
    void reviewedMembers() {
        QFile file(LEGACY_SCOPE_MEMBERS);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.errorString()));
        const auto expected = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
        const auto actual = legacyScopeMembers();
        for (const auto &member : actual)
            if (!expected.contains(member))
                qWarning().noquote() << "Unreviewed legacy scope member:" << member;
        QCOMPARE(actual, expected);
    }

    void outerIds_data() {
        QTest::addColumn<QByteArray>("witness");
        QTest::newRow("root") << QByteArray("rootsIntact");
        QTest::newRow("runtimeRoot") << QByteArray("runtimeRootsIntact");
        QTest::newRow("readProjection") << QByteArray("projectionsIntact");
        QTest::newRow("abandonApply") << QByteArray("abandonsIntact");
        QTest::newRow("declarations") << QByteArray("declarativeScopesIntact");
    }

    void outerIds() {
        QFETCH(QByteArray, witness);
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(LEGACY_SCOPE_FIXTURE));
        const std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QVERIFY(root->property("loaded").toBool());
        QVERIFY(root->property(witness.constData()).toBool());
    }

    void returnedScopes() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(LEGACY_SCOPE_FIXTURE));
        const std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QTRY_VERIFY(root->property("storeReady").toBool());
        bool intact = false;
        QVERIFY(QMetaObject::invokeMethod(root.get(), "returnedScopesIntact",
                                          Q_RETURN_ARG(bool, intact)));
        QVERIFY(intact);
    }
};

QTEST_MAIN(QmlLegacyScopeTest)
#include "test_qml_legacy_scope.moc"
