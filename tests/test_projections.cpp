// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "confignumber.h"
#include <QTemporaryDir>
#include <QSemaphore>
#include <QSignalSpy>
#include <atomic>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class ProjectionTest : public QObject {
    Q_OBJECT
private slots:
    void rejectedLocalProjectionNeverReportsAcceptance_data() {
        QTest::addColumn<int>("operation");
        QTest::newRow("set") << 0;
        QTest::newRow("reset") << 1;
        QTest::newRow("group") << 2;
    }
    void rejectedLocalProjectionNeverReportsAcceptance() {
        QFETCH(int, operation);
        auto n = D::leaf(T::SafeInteger, 0);
        n.admissionPolicy = D::AdmissionPolicy::Strict;
        ConfigProjectionSpec projection{
            "checked", {"user"}, {}, [](const ConfigProjectionInput &input) {
                auto values = input.values;
                if (input.raw.value("n").toInt() != 1)
                    values["n"] = false;
                return ConfigProjectionResult{values, input.compatibilityRaw, {}, {}};
            }};
        ConfigLayers layers(ConfigSchema(D::group({{"n", n}})),
                            {{"user",
                              ConfigLayerKind::File,
                              ConfigAdmissionPolicy::FieldFallback,
                              {{"user", {}, {{"n", 1}}}},
                              true}},
                            {projection});
        const auto before = layers.source("user", "user")->raw;
        const auto revision = layers.info({"n"}).intentRevision;
        const auto result = operation == 0   ? layers.setUser({"n"}, 7)
                            : operation == 1 ? layers.reset({"n"}, "user")
                                             : layers.applyEdits({{{"n"}, QVariant(7)}});
        QVERIFY(!result.accepted);
        QVERIFY(!result.errors.isEmpty());
        QCOMPARE(layers.value({"n"})->toInt(), 1);
        QCOMPARE(layers.source("user", "user")->raw, before);
        QCOMPARE(layers.info({"n"}).intentRevision, revision);
    }
    void authoredWarningsAreNotFailures() {
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        compat::put(file, "{\"n\":1}");
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        o.jsonFiles = {{"user", "user", file}};
        o.projections = {{"warning", {"user"}, {}, [](const ConfigProjectionInput &input) {
                              return ConfigProjectionResult{input.values,
                                                            input.compatibilityRaw,
                                                            {},
                                                            {{"authored-note", {"n"}, "warning"}}};
                          }}};
        ConfigStore store(o);
        QList<ConfigError> warnings;
        QSignalSpy failures(&store, &ConfigStore::sourceError);
        connect(&store, &ConfigStore::sourceWarning, this, [&](const auto &warning) {
            QCOMPARE(store.value({"n"}).toInt(), 1);
            warnings.append(warning);
        });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(!warnings.isEmpty());
        QCOMPARE(warnings.first().code, QString("authored-note"));
        QCOMPARE(warnings.first().operation, QString("projection-warning"));
        QCOMPARE(warnings.first().sourceId, QString("user"));
        QVERIFY(failures.isEmpty());
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        compat::put(file, "malformed");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status, QString("failed"));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(store.value({"n"}).toInt(), 1);
        QVERIFY(!qvariant_cast<ConfigError>(failures.first().first()).empty());
    }
    void coherentBatchAndStrictLayerEffects() {
        auto strict = D::leaf(T::SafeInteger, 0);
        strict.admissionPolicy = D::AdmissionPolicy::Strict;
        const ConfigSchema schema(D::group({{"a", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                            {"b", D::group({{"n", strict}})}}));
        ConfigSource a{"a", {"a"}, {{"n", 1}}, {}, "first-a"};
        ConfigSource b{"b", {"b"}, {{"n", 2}}, {}, "first-b"};
        ConfigProjectionSpec projection{
            "p", {"a"}, {}, [](const ConfigProjectionInput &input) {
                return ConfigProjectionResult{
                    input.values, input.compatibilityRaw, {{"one", 0, "action", {}}}, {}};
            }};
        ConfigLayers layers(
            schema,
            {{"user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {a, b}, true}},
            {projection});
        QCOMPARE(layers.value({"a", "n"})->toInt(), 1);
        QCOMPARE(layers.takeProjectionEffects().size(), 1);
        a.raw["n"] = 3;
        a.revision = "second-a";
        b.raw["n"] = false;
        b.revision = "second-b";
        QVERIFY(!layers.replaceAllSources({{"user", {a, b}}}).accepted);
        QCOMPARE(layers.value({"a", "n"})->toInt(), 1);
        QCOMPARE(layers.value({"b", "n"})->toInt(), 2);
        QVERIFY(layers.takeProjectionEffects().isEmpty());
        QVERIFY(layers.source("user", "b")->raw.value("n").metaType().id() == QMetaType::Bool);
        b.raw["n"] = 4;
        b.revision = "repair-b";
        QVERIFY(layers.replaceAllSources({{"user", {b}}}).accepted);
        QCOMPARE(layers.value({"a", "n"})->toInt(), 3);
        QCOMPARE(layers.value({"b", "n"})->toInt(), 4);
        QCOMPARE(layers.takeProjectionEffects().size(), 1);
    }
    void lowerContextIsFinalAcrossOneBatch() {
        const ConfigSchema schema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
        auto calls = std::make_shared<int>(0);
        ConfigSource lower{"low", {}, {{"n", 1}}, {}, "low-one"};
        ConfigSource upper{"high", {}, {{"opaque", true}}, {}, "high-one"};
        ConfigProjectionSpec projection{
            "p", {"high"}, {}, [calls](const ConfigProjectionInput &input) {
                ++*calls;
                return ConfigProjectionResult{
                    input.lowerValues, input.compatibilityRaw, {{"one", 0, "action", {}}}, {}};
            }};
        ConfigLayers layers(
            schema,
            {{"z-lower", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {lower}},
             {"a-upper", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {upper}}},
            {projection});
        QCOMPARE(*calls, 1);
        layers.takeProjectionEffects();
        lower.raw["n"] = 9;
        lower.revision = "low-two";
        upper.revision = "high-two";
        QVERIFY(layers.replaceAllSources({{"a-upper", {upper}}, {"z-lower", {lower}}}).accepted);
        QCOMPARE(*calls, 2);
        QCOMPARE(layers.value({"n"})->toInt(), 9);
        QCOMPARE(layers.takeProjectionEffects().size(), 1);
    }
    void crossSectionRawOnlyAndRestart() {
        QTemporaryDir dir;
        const QByteArray original = "{\"old\":7,\"wide\":18446744073709551615}";
        compat::put(dir.filePath("old.json"), original);
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"one", D::group({{"n", D::leaf(T::SafeInteger, 0)}})},
                                          {"two", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers = {{"legacy",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"old", {}, {}}}},
                    {"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"one", {"one"}, {}}},
                     true}};
        o.jsonFiles = {{"legacy", "old", dir.filePath("old.json")},
                       {"user", "one", dir.filePath("one.json")}};
        o.projections = {{"move",
                          {"old"},
                          {},
                          [](const ConfigProjectionInput &input) {
                              ConfigProjectionResult result;
                              result.values = {{"one", QVariantMap{{"n", input.raw.value("old")}}},
                                               {"two", QVariantMap{{"n", input.raw.value("old")}}}};
                              result.compatibilityRaw = input.compatibilityRaw;
                              result.compatibilityRaw["rawOnly"] = true;
                              result.effects = {{"effect", 1, "action", {}}};
                              return result;
                          }},
                         {"raw-check", {"old"}, {}, [](const ConfigProjectionInput &input) {
                              if (!input.compatibilityRaw.value("rawOnly").toBool() ||
                                  input.raw.value("wide").value<ConfigNumber>().token() !=
                                      "18446744073709551615")
                                  throw std::exception();
                              return ConfigProjectionResult{
                                  input.values, input.compatibilityRaw, {}, {}};
                          }}};
        ConfigStore store(o);
        int effects = 0;
        connect(&store, &ConfigStore::projectionEffects, this, [&](const auto &) { ++effects; });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"one", "n"}).toInt(), 7);
        QCOMPARE(store.value({"two", "n"}).toInt(), 7);
        QCOMPARE(effects, 2);
        QVERIFY(store.setUser({"one", "n"}, 9).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(store.value({"one", "n"}).toInt(), 9);
        QCOMPARE(store.value({"two", "n"}).toInt(), 7);
        QCOMPARE(effects, 2);
        ConfigStore reopened(o);
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"one", "n"}).toInt(), 9);
        QCOMPARE(reopened.value({"two", "n"}).toInt(), 7);
        QCOMPARE(compat::bytes(dir.filePath("old.json")), original);
        QVERIFY(!compat::bytes(dir.filePath("one.json")).contains("rawOnly"));
        QVERIFY(!compat::bytes(dir.filePath("one.json")).contains("_configqmlVersion"));
    }
    void workerDoesNotBlockTimer() {
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto release = std::make_shared<QSemaphore>();
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
        o.layers = {{"legacy",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"old", {}, {{"n", 1}}}}}};
        o.projections = {
            {"held", {"old"}, {}, [entered, release](const ConfigProjectionInput &input) {
                 entered->store(true);
                 release->acquire();
                 return ConfigProjectionResult{input.values, input.compatibilityRaw, {}, {}};
             }}};
        ConfigStore store(o);
        int beats = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++beats; });
        timer.start(1);
        QTRY_VERIFY(entered->load());
        QTRY_VERIFY(beats >= 5);
        QCOMPARE(store.status(), ConfigStore::Status::Loading);
        release->release();
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"n"}).toInt(), 1);
    }
};
QTEST_GUILESS_MAIN(ProjectionTest)
#include "test_projections.moc"
