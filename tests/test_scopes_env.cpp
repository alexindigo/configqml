// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configscope.h"
#include "configenvironment.h"
#include <QTest>
#include <limits>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class ScopesEnvTest : public QObject {
    Q_OBJECT
private slots:
    void losslessStructuredNumbers_data() {
        QTest::addColumn<QString>("token");
        QTest::addColumn<bool>("unsignedType");
        QTest::addColumn<bool>("accepted");
        QTest::addColumn<QVariant>("expected");
        QTest::newRow("fraction") << QString("1.5") << false << false << QVariant();
        QTest::newRow("near-integer-fraction")
            << QString("1.0000000000000001") << false << false << QVariant();
        QTest::newRow("wide-integer") << QString("9007199254740993") << false << true
                                      << QVariant::fromValue(qint64(9007199254740993LL));
        QTest::newRow("wide-decimal-integer") << QString("9007199254740993.0") << false << true
                                              << QVariant::fromValue(qint64(9007199254740993LL));
        QTest::newRow("signed-overflow")
            << QString("9223372036854775808") << false << false << QVariant();
        QTest::newRow("unsigned-2pow63") << QString("9223372036854775808") << true << true
                                         << QVariant::fromValue(quint64(1) << 63);
        QTest::newRow("unsigned-max") << QString("18446744073709551615") << true << true
                                      << QVariant::fromValue(std::numeric_limits<quint64>::max());
        QTest::newRow("unsigned-overflow")
            << QString("18446744073709551616") << true << false << QVariant();
        QTest::newRow("negative-zero")
            << QString("-0") << false << true << QVariant::fromValue(qint64(0));
        QTest::newRow("exponent") << QString("1e3") << false << true
                                  << QVariant::fromValue(qint64(1000));
    }
    void losslessStructuredNumbers() {
        QFETCH(QString, token);
        QFETCH(bool, unsignedType);
        QFETCH(bool, accepted);
        QFETCH(QVariant, expected);
        auto number = D::leaf(unsignedType ? T::UnsignedInteger : T::Integer,
                              unsignedType ? QVariant::fromValue(quint64(0))
                                           : QVariant::fromValue(qint64(0)));
        auto record = D::leaf(T::Record, QVariantMap{{"n", *number.defaultValue}});
        record.fields = {{"n", number}};
        auto map = D::leaf(T::Map, QVariantMap{});
        map.element = {number};
        ConfigSchema schema(
            D::group({{"list", D::list(number, {})}, {"record", record}, {"map", map}}));
        for (const QString &key : {QString("list"), QString("record"), QString("map")}) {
            const QString text = key == "list" ? "[" + token + "]" : "{\"n\":" + token + "}";
            const auto result = mapEnvironment(schema, {{"VALUE", {key}}}, {{"VALUE", text}});
            QCOMPARE(result.ok(), accepted);
            if (accepted) {
                auto actual = key == "list" ? result.values[key].toList().first()
                                            : result.values[key].toMap()["n"];
                QCOMPARE(actual.metaType(), expected.metaType());
                QCOMPARE(actual, expected);
            } else {
                QVERIFY(!result.errors.isEmpty());
                QCOMPARE(result.errors.first().code, token.startsWith("1.")
                                                         ? QString("fractional-integer")
                                                         : QString("integer-range"));
                QVERIFY(!result.values.contains(key));
            }
        }
    }
    void opaqueScopes() {
        const QString key = "../plugin.with/a_b/λ";
        const auto group = D::group({{"n", D::leaf(T::Integer, 0)}});
        ConfigSchema s(D::group({{"plugins", D::group({{key, group}, {"other", group}})}}));
        auto owner = std::make_shared<ConfigLayers>(
            s, QList<ConfigLayer>{{"user",
                                   ConfigLayerKind::File,
                                   ConfigAdmissionPolicy::FieldFallback,
                                   {{"plugins-file", {"plugins"}, {}}},
                                   true}});
        ConfigScope root(owner);
        auto a = root.scope({"plugins", key});
        auto b = root.scope({"plugins", "other"});
        QVERIFY(a.attached());
        QVERIFY(a.setUser({"n"}, 3).accepted);
        QCOMPARE(a.value({"n"})->toLongLong(), 3);
        QCOMPARE(b.value({"n"})->toLongLong(), 0);
        auto readOnly = a.scope({}, false);
        QVERIFY(!readOnly.setUser({"n"}, 4).accepted);
        QVERIFY(!readOnly.scope({}, true).setUser({"n"}, 4).accepted);
        QVERIFY(!readOnly.info({"n"}).route.valid());
        a.detach();
        QVERIFY(!a.value({"n"}));
        QVERIFY(!a.setUser({"n"}, 4).accepted);
        QCOMPARE(readOnly.value({"n"})->toLongLong(), 3);
        QVERIFY(b.setUser({"n"}, 5).accepted);
        QVERIFY(!root.scope({"not-declared"}).attached());
        auto encoded = encodeScopeKey(key);
        QVERIFY(encoded);
        QVERIFY(!encoded->contains('/'));
        QCOMPARE(*decodeScopeKey(*encoded), key);
        QVERIFY(!decodeScopeKey("../escape"));
        QVERIFY(!decodeScopeKey("k-ff"));
        QVERIFY(!decodeScopeKey("k-2E"));
        QVERIFY(encodeScopeKey("a.b") != encodeScopeKey("a/b"));
        // A plugin sub-store is an ordinary independent engine, not a global registry.
        ConfigScope sub(std::make_shared<ConfigLayers>(s, QList<ConfigLayer>{}));
        QCOMPARE(sub.value({"plugins", key, "n"})->toLongLong(), 0);
    }
    void environment() {
        auto record = D::leaf(T::Record, QVariantMap{{"x", 1}});
        record.fields = {{"x", D::leaf(T::Integer, 0)}};
        ConfigSchema s(
            D::group({{"text", D::leaf(T::String, "default")},
                      {"flag", D::leaf(T::Boolean, false)},
                      {"n", D::leaf(T::Integer, 0)},
                      {"unsigned", D::leaf(T::UnsignedInteger, QVariant::fromValue(quint64(0)))},
                      {"real", D::leaf(T::Real, 0.0)},
                      {"list", D::list(D::leaf(T::Integer, 0), {})},
                      {"record", record},
                      {"camelCase", D::leaf(T::String, "")}}));
        QList<ConfigEnvironmentMapping> mappings{
            {"TEXT", {"text"}}, {"FLAG", {"flag"}}, {"N", {"n"}},          {"U", {"unsigned"}},
            {"REAL", {"real"}}, {"LIST", {"list"}}, {"RECORD", {"record"}}};
        auto mapped = mapEnvironment(s, mappings,
                                     {{"TEXT", ""},
                                      {"FLAG", "true"},
                                      {"N", "9223372036854775807"},
                                      {"U", "18446744073709551615"},
                                      {"REAL", "1.5e2"},
                                      {"LIST", "[1,2]"},
                                      {"RECORD", "{\"x\":3}"},
                                      {"UNDECLARED", "x"}});
        QVERIFY(mapped.ok());
        QCOMPARE(mapped.values["text"].toString(), QString());
        QCOMPARE(mapped.values["n"].toLongLong(), std::numeric_limits<qint64>::max());
        QCOMPARE(mapped.values["unsigned"].toULongLong(), std::numeric_limits<quint64>::max());
        QCOMPARE(mapped.values["list"].toList().size(), 2);
        ConfigLayers e(s, {{"env",
                            ConfigLayerKind::Environment,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"injected", {}, mapped.values}}}});
        QCOMPARE(e.value({"record"})->toMap()["x"].toLongLong(), 3);
        QVERIFY(!e.value({"UNDECLARED"}));
        for (const auto &bad : {QString("1x"), QString(" 1"), QString("1\n"), QString("1.2"),
                                QString("9223372036854775808"), QString("01")})
            QVERIFY(!mapEnvironment(s, {{"N", {"n"}}}, {{"N", bad}}).ok());
        QVERIFY(!mapEnvironment(s, mappings, {{"FLAG", "yes"}, {"LIST", "1,2"}}).ok());
        QVERIFY(!mapEnvironment(s, {{"N", {"n"}}, {"N", {"text"}}}, {}).ok());
        QVERIFY(!mapEnvironment(s, {{"N", {"n"}}, {"OTHER", {"n"}}}, {}).ok());
        QVERIFY(environmentMappings(s, "APP", {{"n"}, {"list"}}));
        QVERIFY(!environmentMappings(s, "APP", {{"camelCase"}}));
        QCOMPARE(mapEnvironment(s, mappings, {}).values.size(), 0);
    }
};
QTEST_GUILESS_MAIN(ScopesEnvTest)
#include "test_scopes_env.moc"
