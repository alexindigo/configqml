// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configschema.h"
#include "confignumber.h"
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
