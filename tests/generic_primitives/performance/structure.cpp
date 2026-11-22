// SPDX-License-Identifier: LGPL-3.0-or-later
// Count the actual old per-leaf rebuilding boundary, not a timing proxy.
#include "configlayers.h"
#include <QTest>
#include <dlfcn.h>
#include <limits>

namespace {
thread_local quint64 individualWrites = 0;
}
namespace configqml {
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
