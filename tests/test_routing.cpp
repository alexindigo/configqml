// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlayers.h"
#include "confignumber.h"
#include "numericfixtures.h"
#include <QTest>
using namespace configqml;
using D = ConfigDescriptor;
class RoutingTest : public QObject {
    Q_OBJECT
private slots:
    void exactNumericWrites() {
        for (const auto &row : numericFixtures()) {
            auto number = D::leaf(row.unsignedType ? D::Type::UnsignedInteger : D::Type::Integer,
                                  row.unsignedType ? QVariant::fromValue(quint64(0))
                                                   : QVariant::fromValue(qint64(0)));
            ConfigLayers engine(ConfigSchema(D::group({{"n", number}})),
                                {{"user",
                                  ConfigLayerKind::File,
                                  ConfigAdmissionPolicy::FieldFallback,
                                  {{"source", {}, {}}},
                                  true}});
            auto raw = decodeJsonValue(row.token.toLatin1());
            QVERIFY(raw.ok());
            auto result = engine.setUser({"n"}, raw.value);
            QCOMPARE(result.accepted, row.expected.isValid());
            if (result.accepted) {
                QCOMPARE(*engine.value({"n"}), row.expected);
                QCOMPARE(engine.info({"n"}).layerId, QString("user"));
            } else {
                QCOMPARE(result.errors.first().code, row.error);
                QCOMPARE(engine.info({"n"}).layerId, QString("defaults"));
            }
        }
    }
    void choicesMasksAndReset() {
        ConfigSchema s(D::group({{"n", D::leaf(D::Type::Integer, 0)}}));
        ConfigLayer user{"user",
                         ConfigLayerKind::File,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"user-file", {}, {{"unknown", 7}}}},
                         true};
        ConfigLayer env{"env",
                        ConfigLayerKind::Environment,
                        ConfigAdmissionPolicy::FieldFallback,
                        {{"injected", {}, {{"n", 9}, {"unknown", 8}}}}};
        ConfigLayer runtime{"runtime",
                            ConfigLayerKind::Runtime,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"overlay", {}, {}}}};
        ConfigLayers e(s, {user, env, runtime});
        auto result = e.setUser({"n"}, 0);
        QVERIFY(result.accepted);
        QCOMPARE(result.maskedBy, QString("env"));
        QCOMPARE(result.description(), QString("masked by env"));
        QCOMPARE(result.route.sourceId, QString("user-file"));
        QCOMPARE(e.value({"n"})->toLongLong(), 9);
        QCOMPARE(e.source("user", "user-file")->raw["n"].toLongLong(), 0);
        QCOMPARE(e.source("user", "user-file")->raw["unknown"].toLongLong(), 7);
        auto info = e.info({"n"});
        QCOMPARE(info.layerId, QString("env"));
        QCOMPARE(info.contributions.size(), 4);
        QCOMPARE(info.contributions[1].maskedBy, QString("env"));
        QVERIFY(info.contributions[1].rawPresent);
        auto revision = result.intentRevision;
        QVERIFY(e.setUser({"n"}, 0).intentRevision > revision);
        QVERIFY(!e.setUser({"n"}, "invalid").accepted);
        QVERIFY(!e.setUser({"unknown"}, 1).accepted);
        QVERIFY(e.setRuntime({"n"}, 12).accepted);
        QCOMPARE(e.value({"n"})->toLongLong(), 12);
        QCOMPARE(e.info({"n"}).maskedBy, QString("runtime"));
        QVERIFY(e.clearRuntime({"n"}).accepted);
        QCOMPARE(e.value({"n"})->toLongLong(), 9);
        QVERIFY(e.reset({"n"}, "user").accepted);
        QVERIFY(!e.source("user", "user-file")->raw.contains("n"));
        QVERIFY(!e.reset({"n"}, "env").accepted);
        auto emptyEnv = *e.source("env", "injected");
        emptyEnv.raw.clear();
        QVERIFY(e.replaceSource("env", emptyEnv).accepted);
        QVERIFY(e.setUser({"n"}, 0).accepted);
        QCOMPARE(e.info({"n"}).layerId, QString("user"));
        QVERIFY(e.reset({"n"}, "user").accepted);
        QCOMPARE(e.info({"n"}).layerId, QString("defaults"));
    }
    void shardsAndReadOnly() {
        auto group = D::group({{"n", D::leaf(D::Type::Integer, 0)}});
        ConfigSchema s(D::group({{"a", group}, {"b", group}}));
        ConfigLayer user{"user",
                         ConfigLayerKind::File,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"a-file", {"a"}, {{"future", 1}}}, {"b-file", {"b"}, {}}},
                         true};
        ConfigLayers e(s, {user});
        QVERIFY(e.valid());
        QCOMPARE(e.setUser({"a", "n"}, 4).route.sourceId, QString("a-file"));
        QCOMPARE(e.setUser({"b", "n"}, 5).route.sourceId, QString("b-file"));
        QVERIFY(!e.source("a-file", "a-file"));
        QCOMPARE(e.source("user", "a-file")->raw["n"].toLongLong(), 4);
        QVERIFY(e.reset({"a", "n"}, "user").accepted);
        QCOMPARE(e.source("user", "a-file")->raw["future"].toLongLong(), 1);
        QCOMPARE(e.value({"b", "n"})->toLongLong(), 5);
        QVERIFY(e.reset({}, "user").accepted); // declared subtree reset spans shards coherently
        QCOMPARE(e.value({"b", "n"})->toLongLong(), 0);
        user.writable = false;
        ConfigLayers readOnly(s, {user});
        QVERIFY(!readOnly.setUser({"a", "n"}, 1).accepted);
        QVERIFY(!readOnly.reset({}, "user").accepted);
        user.writable = true;
        auto duplicate = user;
        duplicate.id = "other";
        QVERIFY(!ConfigLayers(s, {user, duplicate}).valid());
        user.sources.append({"all", {}, {}});
        QVERIFY(!ConfigLayers(s, {user}).valid());
    }
};
QTEST_GUILESS_MAIN(RoutingTest)
#include "test_routing.moc"
