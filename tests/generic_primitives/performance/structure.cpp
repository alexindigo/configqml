// SPDX-License-Identifier: LGPL-3.0-or-later
// Count the actual old per-leaf rebuilding boundary, not a timing proxy.
#include "configlayers.h"
#include "configselection_p.h"
#include "configcandidate_p.h"
#include <QTest>
#include <dlfcn.h>
#include <limits>

namespace {
thread_local quint64 individualWrites = 0;
thread_local quint64 prefixScans = 0;
thread_local quint64 provenanceQueries = 0;
} // namespace
namespace configqml {
ConfigInfo ConfigLayers::info(const ConfigPath &path) const {
    using F = ConfigInfo (*)(const ConfigLayers *, const ConfigPath &);
    static auto real = reinterpret_cast<F>(dlsym(RTLD_NEXT, CONFIGQML_INFO_SYMBOL));
    if (!real)
        qFatal("missing actual info boundary");
    ++provenanceQueries;
    return real(this, path);
}
bool startsWith(const ConfigPath &path, const ConfigPath &prefix) {
    using F = bool (*)(const ConfigPath &, const ConfigPath &);
    static auto real = reinterpret_cast<F>(dlsym(RTLD_NEXT, CONFIGQML_STARTS_WITH_SYMBOL));
    if (!real)
        qFatal("missing actual startsWith boundary");
    ++prefixScans;
    return real(path, prefix);
}
void putValue(QVariantMap &tree, const ConfigPath &path, const QVariant &value) {
    using F = void (*)(QVariantMap &, const ConfigPath &, const QVariant &);
    static auto real = reinterpret_cast<F>(dlsym(RTLD_NEXT, CONFIGQML_PUT_VALUE_SYMBOL));
    if (!real)
        qFatal("missing actual putValue boundary");
    ++individualWrites;
    real(tree, path, value);
}
} // namespace configqml
using namespace configqml;
class LayerStructureTest : public QObject {
    Q_OBJECT
private slots:
    void candidateCapturesSelectedFacts_data() { oneNamespaceAssembly_data(); }
    void candidateCapturesSelectedFacts() {
        QFETCH(int, leaves);
        using D = ConfigDescriptor;
        QMap<QString, D> fields;
        QVariantMap raw;
        for (int i = 0; i < leaves; ++i) {
            const auto key = "field" + QString::number(i);
            fields[key] = D::leaf(D::Type::SafeInteger, 1);
            raw[key] = 4;
        }
        ConfigSchema schema(D::group({{"group", D::group(fields)}}));
        ConfigLayers layers(schema, {{"user",
                                      ConfigLayerKind::File,
                                      ConfigAdmissionPolicy::FieldFallback,
                                      {{"user", {"group"}, raw}},
                                      true},
                                     {"runtime",
                                      ConfigLayerKind::Runtime,
                                      ConfigAdmissionPolicy::FieldFallback,
                                      {{"runtime", {"group"}, {{"field0", 8}}}},
                                      false}});
        const auto selection =
            ConfigSelection::capture(schema, schema.leaves(), schema.leaves(),
                                     ConfigSelection::Target::UserLayer, "selection");
        provenanceQueries = 0;
        const auto candidate =
            resolveCandidate(layers, ConfigEditBatch::capture(selection, {}), {}, {});
        const auto queries = provenanceQueries;
        QVERIFY(candidate.valid());
        QCOMPARE(candidate.facts.size(), leaves);
        for (const auto &path : schema.leaves()) {
            QCOMPARE(candidate.facts.value(path).effective, layers.value(path).value());
            QCOMPARE(candidate.facts.value(path).maskedBy, layers.info(path).maskedBy);
            QVERIFY(candidate.facts.value(path).targetPresent);
        }
        qInfo() << "CANDIDATE_SELECTED_STRUCTURE leaves=" << leaves
                << "provenance_queries=" << queries;
        QCOMPARE(queries, quint64(0));
    }
    void selectionUsesAncestorIndex_data() { oneNamespaceAssembly_data(); }
    void selectionUsesAncestorIndex() {
        QFETCH(int, leaves);
        using D = ConfigDescriptor;
        QMap<QString, D> fields;
        for (int i = 0; i < leaves; ++i)
            fields["field" + QString::number(i)] = D::leaf(D::Type::SafeInteger, 1);
        const ConfigSchema schema(D::group({{"group", D::group(fields)}}));
        const auto allowed = schema.leaves();
        prefixScans = 0;
        const auto selection = ConfigSelection::capture(
            schema, allowed, allowed, ConfigSelection::Target::UserLayer, "selection");
        const auto scans = prefixScans;
        QVERIFY(selection.valid());
        QCOMPARE(selection.leaves, allowed);
        qInfo() << "SELECTION_TREE_STRUCTURE leaves=" << leaves << "prefix_scans=" << scans;
        QCOMPARE(scans, quint64(0));
    }
    void projectionLowerAssembly_data() { oneNamespaceAssembly_data(); }
    void projectionLowerAssembly() {
        QFETCH(int, leaves);
        using D = ConfigDescriptor;
        QMap<QString, D> fields;
        QVariantMap raw, expected;
        for (int i = 0; i < leaves; ++i) {
            const auto key = "field" + QString::number(i);
            fields[key] = D::leaf(D::Type::SafeInteger, 1);
            raw[key] = 4;
            expected[key] = qint64(4);
        }
        const ConfigSchema schema(D::group({{"group", D::group(fields)}}));
        ConfigProjectionSpec projection;
        projection.projectionId = "upper";
        projection.sourceIds = {"upper"};
        int calls = 0;
        QVariantMap capturedLower;
        projection.transform = [&](const ConfigProjectionInput &input) {
            ++calls;
            capturedLower = input.lowerValues;
            ConfigProjectionResult result;
            result.values = input.values;
            result.compatibilityRaw = input.compatibilityRaw;
            return result;
        };
        individualWrites = 0;
        ConfigLayers layers(schema,
                            {{"lower",
                              ConfigLayerKind::File,
                              ConfigAdmissionPolicy::FieldFallback,
                              {{"lower", {"group"}, raw}},
                              true},
                             {"upper",
                              ConfigLayerKind::File,
                              ConfigAdmissionPolicy::FieldFallback,
                              {{"upper", {"group"}, {{"field0", 8}}}},
                              false}},
                            {projection});
        const auto perLeafWrites = individualWrites;
        QVERIFY(layers.valid());
        QCOMPARE(calls, 1);
        QCOMPARE(capturedLower, expected);
        expected["field0"] = qint64(8);
        QCOMPARE(layers.effective(), (QVariantMap{{"group", expected}}));
        QCOMPARE(layers.source("lower", "lower")->raw, raw);
        qInfo() << "PROJECTION_TREE_STRUCTURE leaves=" << leaves
                << "individual_writes=" << perLeafWrites;
        QCOMPARE(perLeafWrites, quint64(0));
    }
    void typedAtomicAndOpaqueLeaves() {
        using D = ConfigDescriptor;
        using T = D::Type;
        auto absent = D::leaf(T::String, QVariant());
        absent.optional = true;
        absent.defaultValue.reset();
        auto nullable = D::leaf(T::String, QVariant::fromValue(nullptr));
        nullable.nullable = true;
        D record(T::Record);
        record.defaultValue = QVariantMap{};
        D map(T::Map);
        map.element = {D::leaf(T::UnsignedInteger, quint64(0))};
        map.defaultValue =
            QVariantMap{{"wide", QVariant::fromValue(std::numeric_limits<quint64>::max())}};
        const QString opaque = QString("a") + QChar(0x1f) + QChar(0x1e) + ".b/__proto__";
        ConfigSchema schema(D::group(
            {{"outer",
              D::group({{"null", nullable},
                        {"absent", absent},
                        {"record", record},
                        {"map", map},
                        {"list", D::list(D::leaf(T::SafeInteger, 0), {})},
                        {opaque, D::leaf(T::Boolean, false)},
                        {"inner",
                         D::group({{"tiny", D::leaf(T::Real, 1e-300)},
                                   {"signed",
                                    D::leaf(T::Integer, std::numeric_limits<qint64>::min())}})}})},
             {"a", D::group({{"b", D::leaf(T::String, "nested")}})}}));
        const QVariantMap raw{{"outer", QVariantMap{{opaque, true}, {"unknown", "kept"}}},
                              {"opaqueSibling", "kept"}};
        ConfigLayers layers(schema, {{"user",
                                      ConfigLayerKind::File,
                                      ConfigAdmissionPolicy::FieldFallback,
                                      {{"user", {}, raw}},
                                      true},
                                     {"runtime",
                                      ConfigLayerKind::Runtime,
                                      ConfigAdmissionPolicy::FieldFallback,
                                      {{"runtime", {"outer"}, {{opaque, false}}}},
                                      false}});
        QVERIFY(layers.valid());
        auto expected = schema.defaults();
        // The runtime mask replaces the user value without materializing absence,
        // unpacking atomic maps/records/lists, or exposing unknown raw siblings.
        QCOMPARE(layers.effective(), expected);
        const auto actual = layers.effective();
        const auto selection =
            ConfigSelection::capture(schema, schema.leaves(), schema.leaves(),
                                     ConfigSelection::Target::UserLayer, "selection");
        const auto candidate =
            resolveCandidate(layers, ConfigEditBatch::capture(selection, {}), {}, {});
        QVERIFY(candidate.valid());
        for (const auto &path : schema.leaves()) {
            QCOMPARE(candidate.facts.value(path).effective,
                     valueAt(actual, path).value_or(QVariant()));
            QCOMPARE(candidate.facts.value(path).maskedBy, layers.info(path).maskedBy);
        }
        QVERIFY(!valueAt(actual, {"outer", "absent"}));
        QVERIFY(isNull(*valueAt(actual, {"outer", "null"})));
        QCOMPARE(valueAt(actual, {"outer", "record"})->toMap(), QVariantMap{});
        QCOMPARE(valueAt(actual, {"outer", "list"})->toList(), QVariantList{});
        QCOMPARE(valueAt(actual, {"outer", "map"})->toMap().value("wide").toULongLong(),
                 std::numeric_limits<quint64>::max());
        QCOMPARE(valueAt(actual, {"outer", "inner", "signed"})->toLongLong(),
                 std::numeric_limits<qint64>::min());
        QCOMPARE(valueAt(actual, {"outer", "inner", "tiny"})->toDouble(), 1e-300);
        QCOMPARE(layers.source("user", "user")->raw, raw);
        QVERIFY(layers.clearRuntime({"outer", opaque}).accepted);
        QCOMPARE(valueAt(layers.effective(), {"outer", opaque})->toBool(), true);
        QVERIFY(layers.reset({"outer", opaque}, "user").accepted);
        QCOMPARE(layers.effective(), expected);
    }
    void oneNamespaceAssembly_data() {
        QTest::addColumn<int>("leaves");
        for (int count : {1, 32, 128, 512})
            QTest::newRow(qPrintable(QString::number(count))) << count;
    }
    void oneNamespaceAssembly() {
        QFETCH(int, leaves);
        using D = ConfigDescriptor;
        QMap<QString, D> fields;
        QVariantMap source, expectedGroup;
        for (int i = 0; i < leaves; ++i) {
            const auto key = "field" + QString::number(i);
            fields[key] = D::leaf(D::Type::SafeInteger, 1);
            source[key] = 4;
            expectedGroup[key] = qint64(4);
        }
        auto absent = D::leaf(D::Type::String, QVariant());
        absent.optional = true;
        absent.defaultValue.reset();
        fields["absent"] = absent;
        const auto schema = ConfigSchema(D::group({{"group", D::group(fields)}}));
        ConfigLayers layers(schema, {{"user",
                                      ConfigLayerKind::File,
                                      ConfigAdmissionPolicy::FieldFallback,
                                      {{"user", {"group"}, source}},
                                      true}});
        QVERIFY(layers.valid());
        individualWrites = 0;
        const auto actual = layers.effective();
        const auto perLeafWrites = individualWrites;
        QCOMPARE(actual, (QVariantMap{{"group", expectedGroup}}));
        qInfo() << "EFFECTIVE_TREE_STRUCTURE leaves=" << leaves
                << "individual_writes=" << perLeafWrites;
        QCOMPARE(perLeafWrites, quint64(0));
        QVERIFY(!valueAt(actual, {"group", "absent"}));
        QVERIFY(layers.setUser({"group", "field0"}, 9).accepted);
        QCOMPARE(layers.value({"group", "field0"})->toInt(), 9);
        QCOMPARE(valueAt(actual, {"group", "field0"})->toInt(), 4); // Detached old tree.
        QVERIFY(layers.reset({"group", "field0"}, "user").accepted);
        QCOMPARE(layers.value({"group", "field0"})->toInt(), 1);
    }
};
QTEST_GUILESS_MAIN(LayerStructureTest)
#include "structure.moc"
