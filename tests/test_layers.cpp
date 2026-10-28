// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlayers.h"
#include "confignumber.h"
#include "numericfixtures.h"
#include <QTest>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
static ConfigSchema schema() {
    auto record = D::leaf(T::Record, QVariantMap{{"x", 1}, {"y", 2}});
    auto optional = D::leaf(T::String, "default");
    optional.nullable = true;
    auto number = D::leaf(T::Integer, 8);
    return ConfigSchema(D::group({{"bar", D::group({{"a", number}, {"b", number}})},
                                  {"list", D::list(number, {1, 2})},
                                  {"record", record},
                                  {"text", optional},
                                  {"flag", D::leaf(T::Boolean, true)}}));
}
static ConfigLayer layer(QString id, ConfigLayerKind kind, QVariantMap raw) {
    return {id, kind, ConfigAdmissionPolicy::FieldFallback, {{id + "-source", {}, raw}}, false};
}
class LayersTest : public QObject {
    Q_OBJECT
private slots:
    void numericRawLayers() {
        for (const auto &row : numericFixtures()) {
            auto number = D::leaf(row.unsignedType ? T::UnsignedInteger : T::Integer,
                                  row.unsignedType ? QVariant::fromValue(quint64(0))
                                                   : QVariant::fromValue(qint64(0)));
            auto raw = decodeJsonValue("{\"n\":" + row.token.toLatin1() + "}");
            QVERIFY(raw.ok());
            ConfigSchema schema(D::group({{"n", number}}));
            ConfigLayers layers(schema, {{"user",
                                          ConfigLayerKind::File,
                                          ConfigAdmissionPolicy::FieldFallback,
                                          {{"file", {}, raw.value.toMap()}},
                                          true}});
            QVERIFY(layers.valid());
            QCOMPARE(layers.source("user", "file")->raw["n"].value<ConfigNumber>().token(),
                     row.token);
            if (row.expected.isValid()) {
                QCOMPARE(*layers.value({"n"}), row.expected);
                QVERIFY(layers.diagnostics("user", "file").isEmpty());
            } else {
                QCOMPARE(layers.diagnostics("user", "file").first().code, row.error);
                QCOMPARE(*layers.value({"n"}), *number.defaultValue);
            }
        }
    }
    void mergeAndPrecedence() {
        auto system =
            layer("system", ConfigLayerKind::File, {{"bar", QVariantMap{{"a", 1}, {"b", 2}}}});
        auto user = layer("user", ConfigLayerKind::File,
                          {{"bar", QVariantMap{{"a", 3}}},
                           {"list", QVariantList{}},
                           {"record", QVariantMap{}},
                           {"text", QString()},
                           {"flag", false}});
        auto env = layer("env", ConfigLayerKind::Environment, {{"bar", QVariantMap{{"a", 4}}}});
        auto runtime = layer("runtime", ConfigLayerKind::Runtime, {{"bar", QVariantMap{{"a", 0}}}});
        ConfigLayers engine(schema(), {system, user, env, runtime});
        QVERIFY(engine.valid());
        QCOMPARE(engine.value({"bar", "a"})->toLongLong(), 0);
        QCOMPARE(engine.value({"bar", "b"})->toLongLong(), 2);
        QCOMPARE(engine.value({"list"})->toList(), QVariantList{});
        QCOMPARE(engine.value({"record"})->toMap(), QVariantMap{});
        QCOMPARE(engine.value({"text"})->toString(), QString());
        QCOMPARE(engine.value({"flag"})->toBool(), false);
        QVERIFY(!ConfigLayers(schema(), {runtime, user}).valid());
        QVERIFY(!ConfigLayers(schema(), {env, env}).valid());
    }
    void emptyNullMissing() {
        ConfigLayers engine(schema(), {layer("user", ConfigLayerKind::File, {})});
        QCOMPARE(engine.value({"bar", "a"})->toLongLong(), 8);
        auto source = *engine.source("user", "user-source");
        source.raw = {{"bar", QVariantMap{}},
                      {"text", QVariant::fromValue(nullptr)},
                      {"list", QVariant::fromValue(nullptr)}};
        QVERIFY(engine.replaceSource("user", source).accepted);
        QCOMPARE(engine.value({"bar", "a"})->toLongLong(), 8);
        QVERIFY(isNull(*engine.value({"text"})));
        QCOMPARE(engine.value({"list"})->toList().size(), 2);
        QCOMPARE(engine.diagnostics("user", "user-source").size(), 1);
        QVERIFY(valueAt(engine.source("user", "user-source")->raw, {"list"}));
        source.exists = false;
        QVERIFY(engine.replaceSource("user", source).accepted);
        QCOMPARE(engine.value({"text"})->toString(), QString("default"));
    }
    void rawAndPolicies() {
        auto user =
            layer("user", ConfigLayerKind::File, {{"bar", QVariantMap{{"a", 3}}}, {"future", 9}});
        ConfigLayers tolerant(schema(), {user});
        auto input = *tolerant.source("user", "user-source");
        input.raw = {{"bar", QVariantMap{{"a", "bad"}, {"b", 6}}},
                     {"future", QVariantMap{{"x", 4}}}};
        input.bytes = "original bytes";
        input.revision = "r2";
        QVERIFY(tolerant.replaceSource("user", input).accepted);
        QCOMPARE(tolerant.value({"bar", "a"})->toLongLong(), 8);
        QCOMPARE(tolerant.value({"bar", "b"})->toLongLong(), 6);
        QCOMPARE(tolerant.source("user", "user-source")->raw, input.raw);
        QCOMPARE(tolerant.source("user", "user-source")->bytes, QByteArray("original bytes"));
        QVERIFY(!tolerant.value({"future"}));
        user.policy = ConfigAdmissionPolicy::RejectInput;
        ConfigLayers strict(schema(), {user});
        QVERIFY(!strict.replaceSource("user", input).accepted);
        QCOMPARE(strict.value({"bar", "a"})->toLongLong(), 3);
        QCOMPARE(strict.source("user", "user-source")->raw, input.raw);
        user.policy = ConfigAdmissionPolicy::RetainLastGood;
        ConfigLayers retained(schema(), {user});
        QVERIFY(retained.replaceSource("user", input).accepted);
        QCOMPARE(retained.value({"bar", "a"})->toLongLong(), 3);
        input.raw = {};
        QVERIFY(retained.replaceSource("user", input).accepted);
        QCOMPARE(retained.value({"bar", "a"})->toLongLong(), 8);
    }
    void independentStores() {
        auto user = layer("user", ConfigLayerKind::File, {{"text", "first"}});
        ConfigLayers a(schema(), {user}), b(schema(), {user});
        auto snapshot = a.effective();
        auto input = *a.source("user", "user-source");
        input.raw["text"] = "second";
        QVERIFY(a.replaceSource("user", input).accepted);
        QCOMPARE(a.value({"text"})->toString(), QString("second"));
        QCOMPARE(b.value({"text"})->toString(), QString("first"));
        QCOMPARE(snapshot["text"].toString(), QString("first"));
    }
};
QTEST_GUILESS_MAIN(LayersTest)
#include "test_layers.moc"
