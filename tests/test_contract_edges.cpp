// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlayers.h"
#include "configenvironment.h"
#include <QTest>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class ContractEdgesTest : public QObject {
    Q_OBJECT
private slots:
    void admittedOriginSurvivesRejectedRaw() {
        ConfigSchema schema(D::group({{"n", D::leaf(T::Integer, 0)}}));
        for (auto policy :
             {ConfigAdmissionPolicy::RejectInput, ConfigAdmissionPolicy::RetainLastGood}) {
            ConfigSource source{"file", {}, {{"n", 3}}, "original", "revision-one"};
            ConfigLayers engine(schema, {{"user", ConfigLayerKind::File, policy, {source}, true}});
            source.raw["n"] = "invalid";
            source.bytes = "new raw";
            source.revision = "revision-two";
            engine.replaceSource("user", source);
            QCOMPARE(engine.value({"n"})->toLongLong(), 3);
            const auto info = engine.info({"n"});
            QCOMPARE(info.contributions[1].revision, QByteArray("revision-one"));
            QVERIFY(!info.contributions[1].valid);
            QCOMPARE(engine.source("user", "file")->revision, QByteArray("revision-two"));
        }
    }
    void descriptorsOwnBorrowedStorage() {
        QChar text[] = {QChar('o'), QChar('n')};
        auto descriptor = D::leaf(T::String, QString("on"));
        descriptor.choices = {QString::fromRawData(text, 2)};
        ConfigSchema schema(D::group({{"x", descriptor}}));
        QVERIFY(schema.valid());
        text[0] = QChar('x');
        QVERIFY(schema.validate({"x"}, QString("on")).ok());
        QVERIFY(!schema.validate({"x"}, QString("xn")).ok());
    }
    void explicitPathsRespectAtomicBoundary() {
        auto record = D::leaf(T::Record, QVariantMap{{"x", 1}});
        record.fields = {{"x", D::leaf(T::Integer, 0)}};
        ConfigLayers engine(ConfigSchema(D::group({{"record", record}})), {});
        QVERIFY(engine.value({"record"}));
        QVERIFY(!engine.value({"record", "x"}));
    }
    void ingressOwnsOpaquePathsAndSourceIds() {
        ConfigSchema schema(D::group({{"n", D::leaf(T::Integer, 0)}}));
        QChar sourceId[] = {QChar('s')};
        ConfigLayer layer{"user",
                          ConfigLayerKind::File,
                          ConfigAdmissionPolicy::FieldFallback,
                          {{QString::fromRawData(sourceId, 1), {}, {}}},
                          true};
        ConfigLayers engine(schema, {layer});
        sourceId[0] = QChar('x');
        QVERIFY(engine.source("user", "s"));
        QChar segment[] = {QChar('n')};
        QVERIFY(engine.setUser({QString::fromRawData(segment, 1)}, 4).accepted);
        segment[0] = QChar('z');
        QCOMPARE(engine.value({"n"})->toLongLong(), 4);
        QCOMPARE(engine.info({"n"}).layerId, QString("user"));
    }
    void nullableStructuredEnvironment() {
        auto list = D::list(D::leaf(T::Integer, 0), {});
        list.nullable = true;
        ConfigSchema schema(D::group({{"list", list}}));
        auto result = mapEnvironment(schema, {{"LIST", {"list"}}}, {{"LIST", "null"}});
        QVERIFY(result.ok());
        QVERIFY(isNull(result.values["list"]));
    }
};
QTEST_GUILESS_MAIN(ContractEdgesTest)
#include "test_contract_edges.moc"
