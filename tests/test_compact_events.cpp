// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configprojection.h"
#include "configevidence_p.h"
#include <QTemporaryDir>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;

class CompactEventsTest : public QObject {
    Q_OBJECT
private slots:
    void nativeIdentities() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QString(120, 'a') + ".json");
        compat::put(path, "{\"n\":1}");
        ConfigJsonBackend backend({{"user", "file", path}});
        const auto read = [&] {
            return *backend.take(backend.submit({ConfigBackendRequest::Operation::Read, "file"}));
        };
        const auto first = read();
        QVERIFY(first.success);
        QVERIFY2(first.revision.token.size() <= 64, "Public revisions must not serialize paths");
        QCOMPARE(read().revision, first.revision);
        const auto detail = first.revision.details();
        QCOMPARE(detail.value("target").toString(), path);
        QVERIFY(!detail.value("parents").toString().isEmpty());
        ConfigEvidenceObservation observation;
        observation.revision = first.revision;
        const auto evidence = retainEvidence(directory.filePath("evidence"), observation);
        QVERIFY(evidence);
        QCOMPARE(
            decodeJsonValue(compat::bytes(evidence->directory + "/revision.json")).value.toMap(),
            detail);
        backend.confirm("file", first.revision);
        QVERIFY(!backend.changed("file"));
        compat::put(path, "{\"n\":2}");
        const auto second = read();
        QVERIFY(second.revision != first.revision);
        QVERIFY(backend.changed("file"));
        const auto stale = backend.take(backend.submit(
            {ConfigBackendRequest::Operation::Publish, "file", "{}", first.revision}));
        QVERIFY(!stale->success);
        QCOMPARE(compat::bytes(path), QByteArray("{\"n\":2}"));
    }
    void projectionIdentities() {
        const ConfigSchema schema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
        const ConfigProjectionSpec spec{
            "projection", {"file"}, {}, [](const ConfigProjectionInput &input) {
                return ConfigProjectionResult{input.values, input.compatibilityRaw, {}, {}};
            }};
        ConfigSource source{"file", {}, {{"n", 1}}, {}, QByteArray(16000, 'r')};
        ConfigLayers layers(
            schema,
            {{"user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {source}}},
            {spec});
        QVERIFY(layers.valid());
        const auto events = layers.takeProjectionEffects();
        QCOMPARE(events.size(), 1);
        QVERIFY(events[0].runId.size() <= 64);
        QVERIFY(events[0].revision.size() <= 64);
        QVERIFY(layers.replaceSource("user", source).accepted);
        QVERIFY(layers.takeProjectionEffects().isEmpty());
        source.revision.append('s');
        QVERIFY(layers.replaceSource("user", source).accepted);
        const auto next = layers.takeProjectionEffects();
        QCOMPARE(next.size(), 1);
        QVERIFY(next[0].runId != events[0].runId);
        QVERIFY(next[0].revision != events[0].revision);
    }
};
QTEST_GUILESS_MAIN(CompactEventsTest)
#include "test_compact_events.moc"
