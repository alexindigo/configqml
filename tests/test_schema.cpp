// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configschema.h"
#include "confignumber.h"
#include "configprojection_p.h"
#include "numericfixtures.h"
#include <QTest>
#include <QUrl>
#include <limits>
#include <cmath>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class SchemaTest : public QObject {
    Q_OBJECT
private slots:
    void realDestinationDomain_data() {
        QTest::addColumn<QVariant>("input");
        QTest::newRow("int") << QVariant(17);
        QTest::newRow("uint") << QVariant(17u);
        QTest::newRow("signed") << QVariant::fromValue(qint64(17));
        QTest::newRow("unsigned") << QVariant::fromValue(quint64(17));
        QTest::newRow("float") << QVariant(17.5f);
        QTest::newRow("double") << QVariant(17.5);
        QTest::newRow("json") << decodeJsonValue("17").value;
        QTest::newRow("wide-signed") << QVariant::fromValue(std::numeric_limits<qint64>::max());
        QTest::newRow("wide-unsigned") << QVariant::fromValue(std::numeric_limits<quint64>::max());
    }
    void realDestinationDomain() {
        QFETCH(QVariant, input);
        auto open = D::leaf(T::Record, QVariantMap{});
        open.open = true;
        const auto admitted = open.validate(QVariantMap{{"number", input}});
        QVERIFY(admitted.ok());
        const auto stored = admitted.value.toMap().value("number");
        const auto original = encodeJsonValue(stored).bytes;
        const auto number = D::leaf(T::Real, 0.0);
        const auto expected = stored.metaType() == QMetaType::fromType<ConfigNumber>()
                                  ? stored.value<ConfigNumber>().real().value.toDouble()
                                  : stored.toDouble();
        for (const auto format : {D::Format::Native, D::Format::Qml}) {
            const auto checked = number.validate(stored, {}, format);
            QVERIFY(checked.ok());
            QCOMPARE(checked.value.metaType().id(), QMetaType::Double);
            QCOMPARE(checked.value.toDouble(), expected);
            auto record = D::leaf(T::Record, QVariantMap{});
            auto map = D::leaf(T::Map, QVariantMap{});
            map.element = {number};
            record.fields = {{"list", D::list(number, {})}, {"map", map}};
            const auto nested = record.validate(
                QVariantMap{{"list", QVariantList{stored}}, {"map", QVariantMap{{"n", stored}}}},
                {}, format);
            QVERIFY(nested.ok());
            QCOMPARE(nested.value.toMap().value("list").toList().first().metaType().id(),
                     QMetaType::Double);
            QCOMPARE(nested.value.toMap().value("map").toMap().value("n").toDouble(), expected);
        }
        QCOMPARE(encodeJsonValue(stored).bytes, original);
    }
    void numericDestinationSafeguards() {
        auto real = D::leaf(T::Real, 17); // Defaults/choices/bounds use the same domain.
        real.minimum = 10;
        real.maximum = 20u;
        real.choices = {17u, 18.5};
        ConfigSchema schema(D::group({{"n", real}}));
        QVERIFY(schema.valid());
        QCOMPARE(schema.defaults().value("n").metaType().id(), QMetaType::Double);
        QVERIFY(schema.validate({"n"}, 17).ok());
        QCOMPARE(real.validate(19).errors.first().code, QString("choice"));
        real.choices.clear();
        QCOMPARE(real.validate(21).errors.first().code, QString("range"));
        for (const auto &wrong :
             QList<QVariant>{QString("17"), true, QVariantMap{}, QVariantList{17}})
            QVERIFY(!real.validate(wrong).ok());
        for (const auto n :
             {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
            QVERIFY(!real.validate(n).ok());
        QVERIFY(!real.validate(decodeJsonValue("1e400").value).ok());
        QVERIFY(!D::leaf(T::Integer, 0).validate(17.0).ok());
        QVERIFY(!D::leaf(T::SafeInteger, 0).validate(17.5, {}, D::Format::Qml).ok());
        QVERIFY(!D::leaf(T::SafeInteger, 0).validate(qint64(9007199254740992LL)).ok());
        QVERIFY(!D::leaf(T::UnsignedInteger, quint64(0)).validate(qint64(-1)).ok());
        QVERIFY(!D::leaf(T::Integer, 0).validate(17, {}, D::Format::Qml).ok());
        const auto atom = decodeJsonValue("9007199254740993").value;
        QVERIFY(!atom.value<ConfigNumber>().exactPresentation());
        const auto integer = D::leaf(T::Integer, 0).validate(atom);
        QVERIFY(integer.ok());
        QCOMPARE(integer.value.toLongLong(), 9007199254740993LL);
        QVERIFY(D::leaf(T::Real, 0.0).validate(atom).ok());
        QCOMPARE(encodeJsonValue(atom).bytes, QByteArray("9007199254740993"));
        const auto zero = D::leaf(T::Real, 0.0).validate(-0.0);
        QVERIFY(zero.ok() && std::signbit(zero.value.toDouble()));
    }
    void unknownNumericPresentationIsLossless() {
        for (const auto &text : {"123", "1.25", "-0", "123.00", "1.25e2", "9007199254740992"}) {
            const auto number = *ConfigNumber::fromJson(text);
            QVERIFY2(number.exactPresentation(), text);
            QCOMPARE(unknownPresentation(QVariant::fromValue(number)).metaType().id(),
                     QMetaType::Double);
        }
        for (const auto &text : {"18446744073709551615", "1e400", "1e-400",
                                 "1.2345678901234567890123456789", "9007199254740993"}) {
            const auto number = *ConfigNumber::fromJson(text);
            QVERIFY2(!number.exactPresentation(), text);
            const auto presented = unknownPresentation(QVariant::fromValue(number));
            QCOMPARE(presented.metaType(), QMetaType::fromType<ConfigNumber>());
            QCOMPARE(encodeJsonValue(presented).bytes, QByteArray(text));
        }
        auto descriptor = D::leaf(T::Record, QVariantMap{});
        descriptor.open = true;
        const auto raw =
            decodeJsonValue(
                "{\"safe\":123,\"nested\":[{\"wide\":18446744073709551615,\"fraction\":1.25}]}")
                .value;
        const auto result = collectionPresentation(descriptor, raw).toMap();
        QCOMPARE(result.value("safe").toDouble(), 123.0);
        const auto nested = result.value("nested").toList()[0].toMap();
        QCOMPARE(nested.value("fraction").toDouble(), 1.25);
        QCOMPARE(nested.value("wide").value<ConfigNumber>().token(),
                 QString("18446744073709551615"));
    }
    void exactNumericAdmission() {
        for (const auto &row : numericFixtures()) {
            auto number = D::leaf(row.unsignedType ? T::UnsignedInteger : T::Integer,
                                  row.unsignedType ? QVariant::fromValue(quint64(0))
                                                   : QVariant::fromValue(qint64(0)));
            ConfigSchema schema(D::group({{"n", number}}));
            QVERIFY(schema.valid());
            auto raw = decodeJsonValue(row.token.toLatin1());
            QVERIFY(raw.ok());
            QCOMPARE(raw.value.value<ConfigNumber>().token(), row.token);
            auto admitted = schema.validate({"n"}, raw.value);
            QCOMPARE(admitted.ok(), row.expected.isValid());
            if (admitted.ok()) {
                QCOMPARE(admitted.value.metaType(), row.expected.metaType());
                QCOMPARE(admitted.value, row.expected);
            } else {
                QCOMPARE(admitted.errors.first().code, row.error);
            }
            auto encoded = encodeJsonValue(raw.value);
            QVERIFY(encoded.ok());
            QCOMPARE(encoded.bytes, row.token.toLatin1());
        }
        ConfigSchema real(D::group({{"n", D::leaf(T::Real, 0.0)}}));
        auto minusZero = real.validate({"n"}, decodeJsonValue("-0").value);
        QVERIFY(minusZero.ok());
        QVERIFY(std::signbit(minusZero.value.toDouble()));
        QCOMPARE(real.validate({"n"}, decodeJsonValue("1e3").value).value.toDouble(), 1000.0);
        QVERIFY(!real.validate({"n"}, decodeJsonValue("1e99999").value).ok());
        ConfigSchema integer(D::group({{"n", D::leaf(T::Integer, 0)}}));
        QVERIFY(!integer.validate({"n"}, 1.5).ok());
        QVERIFY(!integer.validate({"n"}, 1.0).ok()); // Native double never proves integral syntax.
    }
    void jsonValueBoundary() {
        const QByteArray input = "{\"list\":[1.0000000000000001,-0,1e3],\"text\":\"1.5 "
                                 "\\\"x\\\"\",\"unknown\":18446744073709551615}";
        auto decoded = decodeJsonValue(input);
        QVERIFY(decoded.ok());
        auto encoded = encodeJsonValue(decoded.value);
        QVERIFY(encoded.ok());
        QCOMPARE(decodeJsonValue(encoded.bytes).value, decoded.value);
        for (const auto &bad :
             {QByteArray("[01]"), QByteArray("[1,]"), QByteArray("{\"x\":1,\"x\":2}"),
              QByteArray("[+1]"), QByteArray("1 2"), QByteArray("\"bad\\q\""), QByteArray("[1e]"),
              QByteArray("1.")})
            QVERIFY2(!decodeJsonValue(bad).ok(), bad.constData());
        QVERIFY(
            !encodeJsonValue(QVariant::fromValue(std::numeric_limits<double>::infinity())).ok());
        QCOMPARE(encodeJsonValue(-0.0).bytes, QByteArray("-0.0"));
    }
    void descriptorConstraintMatrix() {
        for (const auto type :
             {T::Boolean, T::String, T::Url, T::List, T::Record, T::Map, T::Group}) {
            D value;
            value.type = type;
            value.optional = type != T::Group;
            if (type == T::List || type == T::Map)
                value.element = {D::leaf(T::Integer, 0)};
            value.minimum = 0;
            QVERIFY(!ConfigSchema(D::group({{"x", value}})).valid());
        }
        auto leaf = D::leaf(T::Integer, 0);
        leaf.fields = {{"impossible", D::leaf(T::Integer, 0)}};
        QVERIFY(!ConfigSchema(D::group({{"x", leaf}})).valid());
        leaf.fields.clear();
        leaf.element = {D::leaf(T::Integer, 0)};
        QVERIFY(!ConfigSchema(D::group({{"x", leaf}})).valid());
        leaf.element.clear();
        leaf.nullable = true;
        leaf.minimum = QVariant::fromValue(nullptr);
        QVERIFY(!ConfigSchema(D::group({{"x", leaf}})).valid());
        auto element = D::leaf(T::Integer, 0);
        element.optional = true;
        QVERIFY(!ConfigSchema(D::group({{"x", D::list(element, {})}})).valid());
        auto list = D::list(D::leaf(T::Integer, 0), {1});
        list.choices = {QVariantList{1}};
        ConfigSchema constrained(D::group({{"x", list}}));
        QVERIFY(constrained.valid());
        QVERIFY(constrained.validate({"x"}, decodeJsonValue("[1e0]").value).ok());
        QVERIFY(!constrained.validate({"x"}, QVariantList{2}).ok());
        for (auto type : {T::Record, T::Map}) {
            auto object = D::leaf(type, QVariantMap{{"n", 1}});
            if (type == T::Map)
                object.element = {D::leaf(T::Integer, 0)};
            else
                object.fields = {{"n", D::leaf(T::Integer, 0)}};
            object.choices = {QVariantMap{{"n", 1}}};
            ConfigSchema schema(D::group({{"x", object}}));
            QVERIFY(schema.valid());
            QVERIFY(schema.validate({"x"}, decodeJsonValue("{\"n\":1e0}").value).ok());
            QVERIFY(!schema.validate({"x"}, QVariantMap{{"n", 2}}).ok());
        }
        for (int modifier = 0; modifier < 5; ++modifier) {
            auto group = D::group({{"n", D::leaf(T::Integer, 0)}});
            if (modifier == 0)
                group.optional = true;
            if (modifier == 1)
                group.nullable = true;
            if (modifier == 2)
                group.forbidden = true;
            if (modifier == 3)
                group.defaultValue = QVariantMap{{"n", 0}};
            if (modifier == 4)
                group.maximum = 3;
            QVERIFY(!ConfigSchema(group).valid());
            QVERIFY(!ConfigSchema(D::group({{"nested", group}})).valid());
        }
    }
    void namespaceConstraints() {
        auto group = D::group({{"n", D::leaf(T::Integer, 0)}});
        group.choices = {QVariantMap{{"n", 0}}};
        QVERIFY(!ConfigSchema(group).valid());
        QVERIFY(!ConfigSchema(D::group({{"nested", group}})).valid());
    }
    void nullableChoices() {
        auto value = D::leaf(T::Integer, 0);
        value.nullable = true;
        value.choices = {0};
        ConfigSchema schema(D::group({{"n", value}}));
        QVERIFY(schema.valid());
        QVERIFY(!schema.validate({"n"}, QVariant::fromValue(nullptr)).ok());
        value.choices.append(QVariant::fromValue(nullptr));
        ConfigSchema nullable(D::group({{"n", value}}));
        QVERIFY(nullable.valid());
        QVERIFY(nullable.validate({"n"}, QVariant::fromValue(nullptr)).ok());
    }
    void ownership() {
        char bytes[] = "hello";
        QVariantMap input{{"text", QString::fromRawData(reinterpret_cast<const QChar *>(u"hi"), 2)},
                          {"list", QVariantList{1, 2}}};
        auto owned = ownValue(input);
        QVERIFY(owned.ok());
        input["list"] = QVariantList{9};
        QCOMPARE(owned.value.toMap()["list"].toList().size(), 2);
        Q_UNUSED(bytes);
        QVERIFY(!ownValue(QVariant()).ok());
        QVERIFY(!ownValue(QVariant::fromValue(this)).ok());
        QVERIFY(!ownValue(std::numeric_limits<double>::infinity()).ok());
        QVERIFY(!ownValue(QVariantList{1, QVariant()}).ok());
        QVERIFY(isNull(ownValue(QVariant::fromValue(nullptr)).value));
    }
    void typedLeaves() {
        const QList<QPair<T, QVariant>> cases{
            {T::Boolean, false},
            {T::Integer, 0},
            {T::UnsignedInteger, QVariant::fromValue(std::numeric_limits<quint64>::max())},
            {T::Real, 0.0},
            {T::String, QString()},
            {T::Url, QUrl("https://example.org/a%2Fb")}};
        for (const auto &c : cases) {
            auto d = D::leaf(c.first, c.second);
            ConfigSchema s(D::group({{"x", d}}));
            QVERIFY(s.valid());
            QVERIFY(s.validate({"x"}, c.second).ok());
            QVERIFY(!s.validate({"x"}, QVariantMap{}).ok());
            QVERIFY(!s.validate({"x"}, QVariant::fromValue(nullptr)).ok());
            QVERIFY(!s.validate({"unknown"}, c.second).ok());
        }
        auto n = D::leaf(T::Integer, QVariant::fromValue(std::numeric_limits<qint64>::max()));
        n.minimum = QVariant::fromValue(std::numeric_limits<qint64>::max() - 1);
        QVERIFY(n.validate(*n.defaultValue).ok());
        QVERIFY(!n.validate(QVariant::fromValue(std::numeric_limits<qint64>::max() - 2)).ok());
        QVERIFY(!n.validate("4").ok());
        auto choice = D::leaf(T::String, "top");
        choice.choices = {QString("top"), QString("bottom")};
        QVERIFY(choice.validate("top").ok());
        QVERIFY(!choice.validate("left").ok());
    }
    void structuredAndPresence() {
        auto integer = D::leaf(T::Integer, 0);
        D optional;
        optional.type = T::String;
        optional.optional = true;
        optional.nullable = true;
        auto record = D::leaf(T::Record, QVariantMap{{"n", 0}});
        record.fields = {{"n", integer}, {"label", optional}};
        auto list = D::list(record, {});
        auto map = D::leaf(T::Map, QVariantMap{});
        map.element = {integer};
        ConfigSchema s(
            D::group({{"items", list}, {"record", record}, {"optional", optional}, {"map", map}}));
        QVERIFY(s.valid());
        QVERIFY(!valueAt(s.defaults(), {"optional"}));
        QVERIFY(s.validate({"optional"}, QVariant::fromValue(nullptr)).ok());
        QVERIFY(s.validate({"items"}, QVariantList{}).ok());
        QVERIFY(s.validate({"items"}, QVariantList{QVariantMap{{"n", 3}, {"future", 8}}}).ok());
        QVERIFY(!s.validate({"items"}, QVariantList{QVariantMap{}}).ok());
        QVERIFY(s.validate({"map"}, QVariantMap{{"opaque/a.b_c", 3}}).ok());
        QVERIFY(!s.validate({"map"}, QVariantMap{{"x", "bad"}}).ok());
        QVERIFY(!s.validate({"record", "n"}, 4).ok()); // atomic record
        auto nullable = D::leaf(T::String, QVariant::fromValue(nullptr));
        nullable.nullable = true;
        QVERIFY(ConfigSchema(D::group({{"x", nullable}})).valid());
    }
    void invalidDescriptors() {
        auto n = D::leaf(T::Integer, "wrong");
        QVERIFY(!ConfigSchema(D::group({{"n", n}})).valid());
        n = D::leaf(T::Integer, 3);
        n.minimum = 5;
        QVERIFY(!ConfigSchema(D::group({{"n", n}})).valid());
        D missing;
        QVERIFY(!ConfigSchema(D::group({{"n", missing}})).valid());
        auto badList = D::leaf(T::List, QVariantList{});
        QVERIFY(!ConfigSchema(D::group({{"n", badList}})).valid());
        auto forbidden = D::leaf(T::String, "secret");
        forbidden.forbidden = true;
        QVERIFY(!ConfigSchema(D::group({{"n", forbidden}})).valid());
    }
};
QTEST_GUILESS_MAIN(SchemaTest)
#include "test_schema.moc"
