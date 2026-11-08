// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configdraft.h"
#include "configjsoncodec.h"
#include "configoverlayeditor.h"
#include "storeprobe.h"
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFileInfo>
#include <QDir>
#include <algorithm>
#include <atomic>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
using E = ConfigRecordListEdit;
namespace {
ConfigSchema schema(bool open = false) {
    D row(T::Record);
    row.open = open;
    row.fields = {{"id", D::leaf(T::String, QString())}};
    auto record = row;
    record.defaultValue = QVariantMap{{"id", "default"}};
    D map(T::Map);
    map.element = {row};
    map.defaultValue = QVariantMap{};
    auto inside = record;
    inside.optional = true;
    D extensible = D::leaf(T::Record, QVariantMap{});
    extensible.open = true;
    extensible.fields = {{"inside", inside}};
    return ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 0)},
                                  {"rows", D::list(row, {})},
                                  {"record", record},
                                  {"map", map},
                                  {"open", extensible},
                                  {"group", D::group({{"n", D::leaf(T::SafeInteger, 0)}})}}));
}
ConfigStoreOptions options(const QString &directory, bool strict = true, bool open = false) {
    ConfigStoreOptions o;
    o.schema = schema(open);
    o.autoSave = false;
    ConfigSource source{"user", {}, {}};
    source.unknownData = strict ? ConfigUnknownData::Strict : ConfigUnknownData::Preserve;
    o.layers = {
        {"user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {source}, true}};
    o.evidenceDirectory = directory + "/evidence";
    o.jsonFiles = {{"user", "user", directory + "/user.json"}};
    o.recordListPolicies = {{"rows", {"rows"}, {}, "id"}};
    o.observedPaths = {{"record"}};
    return o;
}
const QByteArray unknown = "{\"n\":1,\"record\":{\"id\":\"r\",\"extra\":18446744073709551615},"
                           "\"rows\":[{\"id\":\"a\",\"extra\":1.0000000000000001}],\"map\":{"
                           "\"opaque\":{\"id\":\"m\",\"extra\":7}},"
                           "\"group\":{\"n\":2,\"rogue\":{\"child\":1}},\"open\":{\"opaque\":{"
                           "\"retain\":true},\"inside\":{\"id\":\"i\",\"extra\":9}},"
                           "\"rogue\":{\"child\":3},\"_configqmlVersion\":0}";
QVariantMap row(const QString &id = "a") { return {{"id", id}, {"extra", 7}}; }
ConfigResult edit(ConfigStore &store, const QList<E> &edits) {
    return compat::request(store, [&] {
        return store.editRecordList("rows",
                                    {store.info({"rows"}).publicationGeneration, {}, edits});
    });
}
} // namespace
class UnknownDataTest : public QObject {
    Q_OBJECT
private slots:
    void invalidSchemaStrictCollections_data() {
        QTest::addColumn<bool>("map");
        QTest::addColumn<int>("elements");
        QTest::addColumn<bool>("json");
        for (bool map : {false, true})
            for (int elements : {0, 2})
                for (bool json : {false, true})
                    QTest::newRow(qPrintable(
                        QString("%1-%2-%3").arg(map ? "map" : "list").arg(elements).arg(json)))
                        << map << elements << json;
    }
    void invalidSchemaStrictCollections() {
        QFETCH(bool, map);
        QFETCH(int, elements);
        QFETCH(bool, json);
        D collection(map ? T::Map : T::List);
        collection.optional = true;
        for (int i = 0; i < elements; ++i)
            collection.element.append(D::leaf(T::Boolean, false));
        const ConfigSchema invalid(D::group({{"items", collection}}));
        QVERIFY(!invalid.valid());
        const QVariant value =
            map ? QVariant(QVariantMap{{"entry", true}}) : QVariant(QVariantList{true});
        ConfigSource source{"strict", {}, {{"items", value}}};
        source.unknownData = ConfigUnknownData::Strict;
        source.json = json;
        ConfigLayers layers(invalid, {{"user",
                                       ConfigLayerKind::File,
                                       ConfigAdmissionPolicy::FieldFallback,
                                       {source},
                                       true}});
        QVERIFY(!layers.valid());
        const auto errors = layers.diagnostics("user", "strict");
        QVERIFY(!errors.isEmpty());
        QCOMPARE(errors.first().code, QString("invalid-schema"));
        QCOMPARE(layers.source("user", "strict")->raw, source.raw);
        QVERIFY(layers.takeUnknownDataDiagnostics().isEmpty());
        const auto checked = invalid.validate({"items"}, value);
        QVERIFY(!checked.ok());
        QCOMPARE(checked.errors.first().code, QString("invalid-schema"));
        const auto closed = closeConfigValue(collection, value, {"items"});
        QCOMPARE(closed.value, value);
        QVERIFY(closed.removed.isEmpty());
    }
    void failedPublicationDefersWarningsAndReusesEvidence() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"n\":1}");
        const QByteArray foreign = "{\"n\":1,\"foreign\":7}";
        auto o = options(dir.path());
        const auto fail = std::make_shared<std::atomic<bool>>(true);
        const auto retainedBeforePublish = std::make_shared<std::atomic<bool>>(false);
        o.fileFault = [fail, retainedBeforePublish, evidence = o.evidenceDirectory,
                       foreign](const QString &phase, const QString &file) {
            if (phase != "rename")
                return QString();
            for (const auto &name : QDir(evidence).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const auto root = evidence + '/' + name;
                if (!QFileInfo::exists(root + "/metadata.json"))
                    continue;
                const auto metadata =
                    decodeJsonValue(compat::bytes(root + "/metadata.json")).value.toMap();
                if (metadata.value("role").toString() == "unknown-data" &&
                    compat::bytes(root + "/bytes") == foreign && compat::bytes(file) == foreign)
                    retainedBeforePublish->store(true);
            }
            return fail->load() ? QString("publish-fault") : QString();
        };
        ConfigStore store(o);
        QList<ConfigError> warnings;
        connect(&store, &ConfigStore::sourceWarning, this,
                [&](const auto &w) { warnings.append(w); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] {
            return store.retainGroup({"warning-retry", {{"n"}}, {{{"n"}, true, 2}}, {}});
        });
        QCOMPARE(capture.status, QString("retained"));
        compat::put(path, foreign);
        const auto first =
            compat::request(store, [&] { return store.saveGroup(capture.groupToken); });
        QCOMPARE(first.error.code, QString("publish-fault"));
        QVERIFY(retainedBeforePublish->load());
        QCOMPARE(compat::bytes(path), foreign);
        QVERIFY(warnings.isEmpty());
        QString reference;
        for (const auto &ref : first.evidence)
            if (ref.role == "unknown-data")
                reference = ref.evidenceId;
        QVERIFY(!reference.isEmpty());
        const auto second =
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); });
        QCOMPARE(second.error.code, QString("publish-fault"));
        QVERIFY(warnings.isEmpty());
        fail->store(false);
        QCOMPARE(
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); }).status,
            QString("saved"));
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(warnings[0].evidence[0].evidenceId, reference);
        int retained = 0;
        const auto states = store.sourceStates();
        for (const auto &ref : states.first().evidence)
            retained += ref.role == "unknown-data";
        QCOMPARE(retained, 1);
        QVERIFY(!decodeJsonValue(compat::bytes(path)).value.toMap().contains("foreign"));
    }
    void uncertainPublicationStillReportsRemoval() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"n\":1}");
        auto o = options(dir.path());
        o.fileFault = [](const QString &phase, const QString &) {
            return phase == "readback" ? QString("readback-fault") : QString();
        };
        ConfigStore store(o);
        QList<ConfigError> warnings;
        connect(&store, &ConfigStore::sourceWarning, this,
                [&](const auto &w) { warnings.append(w); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"n"}, 2).accepted);
        compat::put(path, "{\"n\":1,\"foreign\":7}");
        QCOMPARE(compat::request(store, [&] { return store.save(); }).publicationState,
                 QString("uncertain"));
        QCOMPARE(warnings.size(), 1);
        QVERIFY(!warnings[0].evidence.isEmpty());
        QVERIFY(!decodeJsonValue(compat::bytes(path)).value.toMap().contains("foreign"));
    }
    void strictAutoSaveRidesAlong() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"n\":1,\"unknown\":true}";
        compat::put(path, original);
        auto o = options(dir.path());
        o.autoSave = true;
        o.autoSaveInterval = 10;
        ConfigStore store(o);
        QSignalSpy saved(&store, &ConfigStore::autoSaved);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QTest::qWait(30); // Three coalescing intervals: loading alone must not autosave.
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(saved.size(), 0);
        QCOMPARE(compat::bytes(path), original);
        QVERIFY(store.setUser({"n"}, 9).accepted);
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(qvariant_cast<ConfigResult>(saved[0][0]).status, QString("saved"));
        const auto disk = decodeJsonValue(compat::bytes(path)).value.toMap();
        QVERIFY(!disk.contains("unknown"));
        QCOMPARE(D::leaf(T::SafeInteger, 0).validate(disk.value("n")).value.toInt(), 9);
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(restarted.value({"n"}).toInt(), 9);
    }
    void foreignWarningCapKeepsEvidenceReference() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"n\":1}");
        ConfigStore store(options(dir.path()));
        QList<ConfigError> warnings;
        connect(&store, &ConfigStore::sourceWarning, this,
                [&](const auto &warning) { warnings.append(warning); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"n"}, 2).accepted);
        QVariantMap foreign{{"n", 1}};
        for (int i = 0; i < 70; ++i)
            foreign[QString("unknown%1").arg(i)] = i;
        const auto bytes = encodeJsonValue(foreign).bytes;
        compat::put(path, bytes);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(warnings.size(), 65);
        QCOMPARE(warnings.last().code, QString("unknown-data-truncated"));
        for (const auto &warning : warnings) {
            QVERIFY(!warning.evidence.isEmpty());
            QCOMPARE(compat::bytes(warning.evidence.first().directory + "/bytes"), bytes);
        }
        QVERIFY(!decodeJsonValue(compat::bytes(path)).value.toMap().contains("unknown0"));
    }
    void readOnlyStrictIdentityCannotClosePreservePublication() {
        QTemporaryDir dir;
        auto o = options(dir.path(), false);
        o.jsonFiles.clear();
        o.layers[0].sources[0].raw = {{"record", row()}, {"rogue", true}};
        ConfigSource readOnly{"user", {"group"}, {{"n", 2}, {"unknown", 3}}};
        readOnly.unknownData = ConfigUnknownData::Strict;
        o.layers.prepend(
            {"readonly", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {readOnly}});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.value({"record"}).toMap().contains("extra"));
        QVERIFY(store.setUser({"n"}, 9).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(store.value({"record"}).toMap().contains("extra"));
        QVariantMap published;
        bool observed = false;
        ConfigStoreTestAccess::run(
            store,
            [&](ConfigWorker &worker) {
                const auto id = worker.backend()->submit(
                    {ConfigBackendRequest::Operation::Read, "user", {}, {}});
                published = decodeMemoryValue(worker.backend()->take(id)->bytes);
            },
            this, [&] { observed = true; });
        QTRY_VERIFY(observed);
        QVERIFY(published.contains("rogue"));
        QCOMPARE(published.value("record").toMap(), row());
    }
    void copyReadOnlyAndMaskedRoutes() {
        QTemporaryDir dir;
        for (int kind = 0; kind < 4; ++kind) {
            auto o = options(dir.path(), false);
            const auto rows = *o.schema.descriptor({"rows"});
            auto optionalRows = rows;
            optionalRows.optional = true;
            D bucket = D::leaf(T::Record, QVariantMap{});
            bucket.fields = {{"rows", optionalRows}};
            o.schema = ConfigSchema(D::group({{"source", D::group({{"rows", rows}})},
                                              {"target", D::group({{"bucket", bucket}})}}));
            o.observedPaths.clear();
            o.jsonFiles.clear();
            o.layers = {{"readonly",
                         ConfigLayerKind::File,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"source", {"source"}, {{"rows", QVariantList{row()}}}}}},
                        {"user",
                         ConfigLayerKind::File,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"target", {"target"}, {}}},
                         kind != 3}};
            if (kind == 1 || kind == 2) {
                const QVariantMap raw =
                    kind == 1
                        ? QVariantMap{{"source", QVariantMap{{"rows", QVariantList{row("mask")}}}}}
                        : QVariantMap{{"target", QVariantMap{{"bucket", QVariantMap{}}}}};
                o.layers.append({"runtime",
                                 ConfigLayerKind::Runtime,
                                 ConfigAdmissionPolicy::FieldFallback,
                                 {{"runtime", {}, raw}}});
            }
            o.recordListPolicies = {
                {"source", {"source", "rows"}, {}, "id"},
                {"target", {"target", "bucket"}, {{ConfigValueSelector::Kind::Key, "rows"}}, "id"}};
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            QVERIFY(!store.info({"source", "rows"}).provenance.route.valid());
            const auto sourceGeneration = store.info({"source", "rows"}).pathGeneration;
            E op;
            op.op = E::Op::Copy;
            op.from = {"source", {}, store.info({"source", "rows"}).publicationGeneration};
            op.destination = {"target",
                              {{ConfigValueSelector::Kind::Key, "rows"}},
                              store.info({"target", "bucket"}).publicationGeneration,
                              0,
                              true};
            const auto result = compat::request(store, [&] {
                return store.editRecordList(
                    "target", {op.destination.generation, op.destination.listAddress, {op}});
            });
            if (kind == 0) {
                QCOMPARE(result.status, QString("admitted"));
                const auto copied = store.value({"target", "bucket"}).toMap().value("rows");
                QCOMPARE(copied.toList()[0].toMap(), row());
                QCOMPARE(store.info({"source", "rows"}).pathGeneration, sourceGeneration);
            } else {
                QCOMPARE(result.error.code,
                         QString(kind == 3 ? "no-write-route" : "list-edit-masked"));
            }
        }
    }
    void closureAcrossSourceKinds() {
        for (const auto kind :
             {ConfigLayerKind::File, ConfigLayerKind::Environment, ConfigLayerKind::Runtime}) {
            ConfigSource source{"source", {}, decodeJsonValue(unknown).value.toMap()};
            source.unknownData = ConfigUnknownData::Strict;
            ConfigLayers layers(schema(), {{"layer",
                                            kind,
                                            ConfigAdmissionPolicy::FieldFallback,
                                            {source},
                                            kind == ConfigLayerKind::File}});
            QVERIFY(layers.valid());
            QVERIFY(!layers.source("layer", "source")->raw.contains("rogue"));
            QVERIFY(!layers.value({"record"})->toMap().contains("extra"));
            QCOMPARE(layers.takeUnknownDataDiagnostics().size(), 6);
        }
    }
    void copyTransferAndAtRespectEveryRoute() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        const auto rows = *o.schema.descriptor({"rows"});
        auto optionalRows = rows;
        optionalRows.optional = true;
        D bucket = D::leaf(T::Record, QVariantMap{});
        bucket.fields = {{"rows", optionalRows}};
        o.schema =
            ConfigSchema(D::group({{"legacy", D::group({{"rows", rows}})},
                                   {"strict", D::group({{"rows", rows}, {"bucket", bucket}})}}));
        ConfigSource legacy{"legacy", {"legacy"}, {{"rows", QVariantList{row()}}}};
        ConfigSource strict{
            "strict", {"strict"}, {{"rows", QVariantList{QVariantMap{{"id", "clean"}}}}}};
        strict.unknownData = ConfigUnknownData::Strict;
        o.layers[0].sources = {legacy, strict};
        o.jsonFiles.clear();
        o.observedPaths.clear();
        o.recordListPolicies = {
            {"legacy", {"legacy", "rows"}, {}, "id"},
            {"strict", {"strict", "rows"}, {}, "id"},
            {"bucket", {"strict", "bucket"}, {{ConfigValueSelector::Kind::Key, "rows"}}, "id"}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto endpoint = [&](const QString &id, bool ensure = false) {
            const auto policy =
                *std::find_if(o.recordListPolicies.cbegin(), o.recordListPolicies.cend(),
                              [&](const auto &p) { return p.policyId == id; });
            return ConfigRecordListEndpoint{
                id, policy.listPath, store.info(policy.path).publicationGeneration, 0, ensure};
        };
        const auto submit = [&](const QList<E> &edits) {
            return compat::request(store, [&] {
                return store.editRecordList("legacy", {endpoint("legacy").generation, {}, edits});
            });
        };
        E op;
        op.op = E::Op::Copy;
        op.from = endpoint("legacy");
        op.destination = endpoint("bucket", true);
        QCOMPARE(submit({op}).error.code, QString("undeclared-member"));
        QVERIFY(store.value({"strict", "bucket"}).toMap().isEmpty());
        op.op = E::Op::Transfer;
        op.from = endpoint("legacy");
        op.destination = endpoint("strict");
        QCOMPARE(submit({op}).error.code, QString("undeclared-member"));
        QCOMPARE(store.value({"legacy", "rows"}).toList().size(), 1);
        E first{E::Op::Set, 0, "legacyAdded", true};
        E second{E::Op::Set, 0, "undeclared", true};
        second.at = endpoint("strict");
        QCOMPARE(submit({first, second}).error.code, QString("undeclared-member"));
        QVERIFY(!store.value({"legacy", "rows"}).toList()[0].toMap().contains("legacyAdded"));
        op.from = endpoint("strict");
        op.destination = endpoint("bucket", true);
        QCOMPARE(submit({op}).status, QString("admitted"));
        QVERIFY(store.value({"strict", "rows"}).toList().isEmpty());
        const auto moved = store.value({"strict", "bucket"}).toMap().value("rows").toList();
        QCOMPARE(moved[0].toMap().value("id").toString(), QString("clean"));
    }
    void overlayOverrideClosure() {
        QTemporaryDir dir;
        for (bool open : {false, true}) {
            auto o = options(dir.path(), true, open);
            o.jsonFiles.clear();
            ConfigStore base(o), store(o);
            QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            ConfigOverlayOptions selected;
            selected.base = base.scope({"group"});
            selected.ownerPath = {"record"};
            selected.ownerGeneration = store.info({"record"}).publicationGeneration;
            selected.identityPath = {{ConfigValueSelector::Kind::Key, "id"}};
            selected.identityValue = QString("default");
            selected.overrideKey = "instanceSettings";
            ConfigOverlayEditor *editor = nullptr;
            connect(&store, &ConfigStore::overlayOpened, this,
                    [&](quint64, auto *opened) { editor = opened; });
            const auto opened = compat::request(store, [&] { return store.openOverlay(selected); });
            if (!open) {
                QCOMPARE(opened.error.code, QString("undeclared-member"));
            } else {
                QCOMPARE(opened.status, QString("opened"));
                QVERIFY(editor);
                QVERIFY(editor->setValue({"n"}, 4).accepted);
                QCOMPARE(compat::request(store, [&] { return editor->apply(); }).status,
                         QString("saved"));
                const auto overrides = store.value({"record"}).toMap().value("instanceSettings");
                QCOMPARE(overrides.toMap().value("n").toInt(), 4);
            }
        }
    }
    void fallbackUsesClosedRaw() {
        const auto s = schema();
        for (bool strict : {false, true}) {
            ConfigSource target{"user", {}, {{"unknown", 1}}};
            target.unknownData = strict ? ConfigUnknownData::Strict : ConfigUnknownData::Preserve;
            ConfigSource fallback{"fallback", {}, {{"n", 8}}};
            fallback.fallbackFor = "user";
            ConfigLayers layers(s, {{"legacy",
                                     ConfigLayerKind::File,
                                     ConfigAdmissionPolicy::FieldFallback,
                                     {fallback}},
                                    {"user",
                                     ConfigLayerKind::File,
                                     ConfigAdmissionPolicy::FieldFallback,
                                     {target},
                                     true}});
            QVERIFY(layers.valid());
            QCOMPARE(layers.fallbackState("fallback"), QString(strict ? "active" : "suppressed"));
            QCOMPARE(layers.value({"n"})->toInt(), strict ? 8 : 0);
        }
        QTemporaryDir dir;
        auto o = options(dir.path());
        ConfigSource fallback{"fallback", {}, {{"n", 8}}};
        fallback.fallbackFor = "user";
        o.layers.prepend(
            {"legacy", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {fallback}});
        o.jsonFiles[0].deleteWhenEmpty = true;
        compat::put(dir.filePath("user.json"), "{\"future\":9}");
        {
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            QCOMPARE(store.value({"n"}).toInt(), 8);
            QVERIFY(store.reset({"n"}, "user").accepted);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
            QVERIFY(!QFileInfo::exists(dir.filePath("user.json")));
            QCOMPARE(store.value({"n"}).toInt(), 8);
        }
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(restarted.value({"n"}).toInt(), 8);
    }
    void memoryPublicationIsCanonical() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.jsonFiles.clear();
        o.layers[0].sources[0].raw = decodeJsonValue(unknown).value.toMap();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"n"}, 9).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVariantMap published;
        bool observed = false;
        ConfigStoreTestAccess::run(
            store,
            [&](ConfigWorker &worker) {
                const auto id = worker.backend()->submit(
                    {ConfigBackendRequest::Operation::Read, "user", {}, {}});
                published = decodeMemoryValue(worker.backend()->take(id)->bytes);
            },
            this, [&] { observed = true; });
        QTRY_VERIFY(observed);
        QVERIFY(!published.contains("rogue"));
        QCOMPARE(published.value("n").toInt(), 9);
        QVERIFY(!published.value("record").toMap().contains("extra"));
    }
    void retainedCanonicalRecovery() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, unknown);
        auto o = options(dir.path());
        const auto fail = std::make_shared<std::atomic<bool>>(true);
        o.fileFault = [fail](const QString &phase, const QString &) {
            return phase == "readback" && fail->exchange(false) ? QString("readback-fault")
                                                                : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto capture = compat::request(store, [&] {
            return store.retainGroup({"canonical", {{"n"}}, {{{"n"}, true, 5}}, {}});
        });
        QCOMPARE(capture.status, QString("retained"));
        const auto failed =
            compat::request(store, [&] { return store.saveGroup(capture.groupToken); });
        QCOMPARE(failed.publicationState, QString("uncertain"));
        const auto canonical = compat::bytes(path);
        QVERIFY(!decodeJsonValue(canonical).value.toMap().contains("rogue"));
        QCOMPARE(
            compat::request(store, [&] { return store.retryGroup(capture.groupToken); }).status,
            QString("saved"));
        QCOMPARE(compat::bytes(path), canonical);
        QCOMPARE(store.value({"n"}).toInt(), 5);
    }
    void structuralClosureAndDeclarationChecks() {
        const auto s = schema();
        QVERIFY(s.valid());
        ConfigSource source{"source", {}, decodeJsonValue(unknown).value.toMap()};
        source.unknownData = ConfigUnknownData::Strict;
        ConfigLayers engine(s, {{"user",
                                 ConfigLayerKind::File,
                                 ConfigAdmissionPolicy::FieldFallback,
                                 {source},
                                 true}});
        QVERIFY(engine.valid());
        const auto raw = engine.source("user", "source")->raw;
        QVERIFY(!raw.contains("rogue"));
        QVERIFY(raw.contains("_configqmlVersion"));
        QVERIFY(!raw.value("group").toMap().contains("rogue"));
        QVERIFY(raw.value("open").toMap().contains("opaque"));
        QVERIFY(!raw.value("open").toMap().value("inside").toMap().contains("extra"));
        QVERIFY(!engine.value({"rows"})->toList()[0].toMap().contains("extra"));
        QVERIFY(!raw.value("map").toMap().value("opaque").toMap().contains("extra"));
        const auto diagnostics = engine.takeUnknownDataDiagnostics();
        QCOMPARE(diagnostics.size(), 6);
        for (const auto &diagnostic : diagnostics)
            QCOMPARE(diagnostic.second.code, QString("unknown-data-removed"));
        QVERIFY(engine.replaceSource("user", source).accepted);
        QVERIFY(engine.takeUnknownDataDiagnostics().isEmpty());
        // Type mismatches, null and invalid declared values survive structural closure.
        const auto invalid =
            closeConfigValue(*s.descriptor({}),
                             QVariantMap{{"record", QVariant::fromValue(nullptr)},
                                         {"rows", 7},
                                         {"map", QVariantMap{{"x", QVariantMap{{"extra", 1}}}}}},
                             {}, true);
        QVERIFY(isNull(invalid.value.toMap().value("record")));
        QCOMPARE(invalid.value.toMap().value("rows").toInt(), 7);
        QCOMPARE(invalid.value.toMap().value("map").toMap().value("x").toMap(), QVariantMap{});
        for (int kind = 0; kind < 3; ++kind) {
            auto root = *s.descriptor({});
            if (kind == 0)
                root.fields["record"].fields.clear();
            if (kind == 1)
                root.fields["record"].defaultValue = row();
            if (kind == 2)
                root.fields["n"].open = true;
            const ConfigSchema bad(root);
            ConfigLayers rejected(bad, {{"user",
                                         ConfigLayerKind::File,
                                         ConfigAdmissionPolicy::FieldFallback,
                                         {source},
                                         true}});
            QVERIFY(!rejected.valid());
            QCOMPARE(rejected.errors().first().code,
                     QString(kind == 2 ? "descriptor" : "unknown-data-declaration"));
        }
        // Strict/projection declarations remain independent.
        ConfigProjectionSpec projection;
        projection.projectionId = "identity";
        projection.sourceIds = {"source"};
        QVariantMap projectionInput;
        projection.transform = [&](const ConfigProjectionInput &in) {
            projectionInput = in.raw;
            ConfigProjectionResult out;
            out.values = in.values;
            out.compatibilityRaw = in.raw;
            return out;
        };
        ConfigLayers projected(
            s,
            {{"user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {source}, true}},
            {projection});
        QVERIFY(projected.valid());
        auto expectedProjection = raw;
        expectedProjection.remove("_configqmlVersion"); // Exposed separately as priorVersion.
        QCOMPARE(projectionInput, expectedProjection);
        QVERIFY(!projectionInput.contains("rogue"));
        QVERIFY(!projectionInput.value("record").toMap().contains("extra"));
        QVERIFY(projectionInput.value("open").toMap().contains("opaque"));
        D assigned;
        assigned = *s.descriptor({"open"});
        QVERIFY(assigned.open);
    }
    void rideAlongWarningsEvidenceAndColdRestart() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, unknown);
        auto o = options(dir.path());
        {
            ConfigStore store(o);
            QList<ConfigError> warnings;
            QList<ConfigSourceAccepted> events;
            connect(&store, &ConfigStore::sourceWarning, this,
                    [&](const auto &w) { warnings.append(w); });
            connect(&store, &ConfigStore::sourceAccepted, this,
                    [&](const auto &e) { events.append(e); });
            QSignalSpy errors(&store, &ConfigStore::sourceError);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            QCOMPARE(warnings.size(), 6);
            QCOMPARE(errors.size(), 0);
            QCOMPARE(warnings.first().operation, QString("unknown-data"));
            QCOMPARE(warnings.first().sourceId, QString("user"));
            QCOMPARE(warnings.first().file, path);
            QVERIFY(!store.value({"record"}).toMap().contains("extra"));
            QVERIFY(!store.presentation({"rows"}).toList()[0].toMap().contains("extra"));
            QVERIFY(!events.first().facts.first().value.toMap().contains("extra"));
            QCOMPARE(compat::bytes(path), unknown);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                     QString("nothing_to_save"));
            QCOMPARE(compat::bytes(path), unknown);
            QVERIFY(store.setUser({"n"}, 3).accepted);
            const auto saved = compat::request(store, [&] { return store.save(); });
            QCOMPARE(saved.status, QString("saved"));
            bool retained = false;
            for (const auto &ref : saved.evidence)
                if (ref.role == "unknown-data") {
                    retained = true;
                    QCOMPARE(compat::bytes(ref.directory + "/bytes"), unknown);
                    QVERIFY(QFileInfo::exists(ref.directory + "/metadata.json"));
                }
            QVERIFY(retained);
            QVERIFY(!store.sourceStates().first().evidence.isEmpty());
            const auto disk = decodeJsonValue(compat::bytes(path)).value.toMap();
            QVERIFY(!disk.contains("rogue"));
            QVERIFY(!disk.value("record").toMap().contains("extra"));
            QVERIFY(disk.value("open").toMap().contains("opaque"));
        }
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(restarted.value({"n"}).toInt(), 3);
        QVERIFY(!restarted.value({"record"}).toMap().contains("extra"));
    }
    void foreignAdditionsAndPreimageConflict_data() {
        QTest::addColumn<bool>("preimage");
        QTest::newRow("path-intent") << false;
        QTest::newRow("strict-preimage") << true;
    }
    void foreignAdditionsAndPreimageConflict() {
        QFETCH(bool, preimage);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, "{\"n\":1}");
        auto o = options(dir.path());
        if (preimage)
            o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictSourcePreimage;
        ConfigStore store(o);
        QList<ConfigError> warnings;
        connect(&store, &ConfigStore::sourceWarning, this,
                [&](const auto &w) { warnings.append(w); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"n"}, 2).accepted);
        const QByteArray foreign = "{\"n\":1,\"foreign\":18446744073709551615}";
        compat::put(path, foreign);
        const auto saved = compat::request(store, [&] { return store.save(); });
        if (preimage) {
            QCOMPARE(saved.error.code, QString("source-preimage-conflict"));
            QCOMPARE(compat::bytes(path), foreign);
            QVERIFY(warnings.isEmpty());
        } else {
            QCOMPARE(saved.status, QString("saved"));
            QCOMPARE(warnings.size(), 1);
            QCOMPARE(warnings.first().path, ConfigPath({"foreign"}));
            QVERIFY(!warnings.first().evidence.isEmpty());
            QCOMPARE(compat::bytes(warnings.first().evidence.first().directory + "/bytes"),
                     foreign);
            QVERIFY(!decodeJsonValue(compat::bytes(path)).value.toMap().contains("foreign"));
        }
    }
    void ingressGroupsRuntimeAndOpenWrites() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        ConfigSource runtime{"runtime", {}, {}};
        runtime.unknownData = ConfigUnknownData::Strict;
        o.layers.append(
            {"runtime", ConfigLayerKind::Runtime, ConfigAdmissionPolicy::FieldFallback, {runtime}});
        o.jsonFiles.clear();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto generation = store.info({"record"}).pathGeneration;
        for (const auto &handle :
             {store.setUser({"record"}, row()), store.setRuntime({"record"}, row()),
              store.initializeIfMissing({"record"}, row())}) {
            QVERIFY(!handle.accepted);
            QCOMPARE(handle.errors.first().code, QString("undeclared-member"));
        }
        QCOMPARE(store.inputs().size(), 0);
        QCOMPARE(store.info({"record"}).pathGeneration, generation);
        const auto retained = compat::request(store, [&] {
            return store.retainGroup({"invalid", {{"record"}}, {{{"record"}, true, row()}}, {}});
        });
        QCOMPARE(retained.error.code, QString("undeclared-member"));
        auto *draft = store.draft({{"record"}});
        QVERIFY(draft->setValue({"record"}, row()).accepted);
        QCOMPARE(compat::request(store, [&] { return draft->apply(); }).error.code,
                 QString("undeclared-member"));
        QVERIFY(draft->dirty());
        QVERIFY(store.setUser({"open"}, QVariantMap{{"anything", row()}}).accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        auto openOptions = options(dir.path(), true, true);
        openOptions.jsonFiles.clear();
        ConfigStore open(openOptions);
        QTRY_COMPARE(open.status(), ConfigStore::Status::Ready);
        QVERIFY(open.setUser({"record"}, row()).accepted);
    }
    void recordListRefusalsAndOpenAcceptance() {
        QTemporaryDir dir;
        for (bool open : {false, true}) {
            auto o = options(dir.path(), true, open);
            o.jsonFiles.clear();
            o.layers[0].sources[0].raw = {{"rows", QVariantList{QVariantMap{{"id", "a"}}}}};
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            const auto inserted = edit(store, {{E::Op::Insert, 1, {}, row("new")}});
            QCOMPARE(inserted.status, QString(open ? "admitted" : "failed"));
            if (!open)
                QCOMPARE(inserted.error.code, QString("undeclared-member"));
            const auto set = edit(store, {{E::Op::Set, 0, "extra", 3}});
            QCOMPARE(set.status, QString(open ? "admitted" : "failed"));
            if (!open)
                QCOMPARE(set.error.code, QString("undeclared-member"));
        }
    }
    void warningsCapRedactionAndReadOnly() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        auto root = *o.schema.descriptor({});
        root.fields["record"].sensitive = true;
        o.schema = ConfigSchema(root);
        o.layers[0].writable = false;
        QVariantMap raw{{"record", QVariantMap{{"id", "a"}, {"PRIVATE_MEMBER", 3}}}};
        for (int i = 0; i < 70; ++i)
            raw[QString("z%1").arg(i, 2, 10, QChar('0'))] = i;
        const auto bytes = encodeJsonValue(raw).bytes;
        compat::put(dir.filePath("user.json"), bytes);
        ConfigStore store(o);
        QList<ConfigError> warnings;
        connect(&store, &ConfigStore::sourceWarning, this,
                [&](const auto &w) { warnings.append(w); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(warnings.size(), 65);
        QCOMPARE(warnings.last().code, QString("unknown-data-truncated"));
        QVERIFY(warnings.first().path.contains("<redacted>"));
        for (const auto &warning : warnings)
            QVERIFY(!warning.path.contains("PRIVATE_MEMBER"));
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(warnings.size(), 65);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(compat::bytes(dir.filePath("user.json")), bytes);
    }
    void deleteUnknownOnlyAndEvidenceDeclaration() {
        QTemporaryDir dir;
        auto o = options(dir.path());
        o.evidenceDirectory.clear();
        {
            ConfigStore invalid(o);
            QTRY_COMPARE(invalid.status(), ConfigStore::Status::Error);
            QCOMPARE(invalid.error(), QString("evidence-directory"));
        }
        o.evidenceDirectory = dir.filePath("evidence");
        o.jsonFiles[0].deleteWhenEmpty = true;
        const auto retainedBeforeDelete = std::make_shared<std::atomic<bool>>(false);
        o.fileFault = [retainedBeforeDelete, evidence = o.evidenceDirectory](const QString &phase,
                                                                             const QString &file) {
            if (phase == "delete")
                for (const auto &name :
                     QDir(evidence).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    const auto root = evidence + '/' + name;
                    if (!QFileInfo::exists(root + "/metadata.json"))
                        continue;
                    const auto metadata =
                        decodeJsonValue(compat::bytes(root + "/metadata.json")).value.toMap();
                    if (metadata.value("role").toString() == "unknown-data" &&
                        compat::bytes(root + "/bytes") == QByteArray("{\"unknown\":true}") &&
                        QFileInfo::exists(file))
                        retainedBeforeDelete->store(true);
                }
            return QString();
        };
        compat::put(dir.filePath("user.json"), "{\"unknown\":true}");
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.reset({"n"}, "user").accepted);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(!QFileInfo::exists(dir.filePath("user.json")));
        QVERIFY(retainedBeforeDelete->load());
    }
    void migrationSeesLegacyAndKeepsOriginal() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original = "{\"oldName\":7,\"leftover\":8}";
        compat::put(path, original);
        auto o = options(dir.path());
        o.schema =
            ConfigSchema(*o.schema.descriptor({}), 1, {{0, [](const QVariantMap &raw) {
                                                            auto out = raw;
                                                            out["n"] = out.take("oldName");
                                                            return ConfigValueResult{out, {}};
                                                        }}});
        ConfigStore store(o);
        QList<ConfigError> warnings;
        connect(&store, &ConfigStore::sourceWarning, this,
                [&](const auto &w) { warnings.append(w); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"n"}).toInt(), 7);
        QVERIFY(!decodeJsonValue(compat::bytes(path)).value.toMap().contains("leftover"));
        QCOMPARE(compat::bytes(path + ".configqml-v0.bak"), original);
        QVERIFY(!warnings.isEmpty());
    }
    void preserveNewMembersRoundTrip() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        compat::put(path, unknown);
        auto o = options(dir.path(), false);
        {
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            QVERIFY(store.setUser({"n"}, 3).accepted);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
            const auto first = decodeJsonValue(compat::bytes(path)).value.toMap();
            QVERIFY(first.contains("rogue"));
            QVERIFY(first.value("group").toMap().contains("rogue"));
            QVERIFY(first.value("map").toMap().value("opaque").toMap().contains("extra"));
            QVERIFY(compat::bytes(path).contains("1.0000000000000001"));
            QVERIFY(compat::bytes(path).contains("18446744073709551615"));
        }
        {
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            const auto carried = store.value({"record"}).toMap().value("extra");
            QCOMPARE(carried.value<ConfigNumber>().token(), QString("18446744073709551615"));
            const auto map = store.value({"map"}).toMap().value("opaque").toMap();
            QCOMPARE(map.value("extra").value<ConfigNumber>().token(), QString("7"));
            QCOMPARE(edit(store, {{E::Op::Insert, 1, {}, row("new")}}).status, QString("admitted"));
            QCOMPARE(edit(store, {{E::Op::Set, 0, "authored", true}}).status, QString("admitted"));
            QVERIFY(store.setUser({"record"}, QVariantMap{{"id", "new"}, {"newWholeLeaf", true}})
                        .accepted);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        }
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        const auto rows = restarted.value({"rows"}).toList();
        QCOMPARE(rows.size(), 2);
        QVERIFY(rows[0].toMap().value("authored").toBool());
        QCOMPARE(rows[0].toMap().value("extra").value<ConfigNumber>().token(),
                 QString("1.0000000000000001"));
        QCOMPARE(rows[1].toMap().value("extra").value<ConfigNumber>().token(), QString("7"));
        QVERIFY(restarted.value({"record"}).toMap().value("newWholeLeaf").toBool());
        const auto raw = decodeJsonValue(compat::bytes(path)).value.toMap();
        QVERIFY(raw.contains("rogue"));
        QVERIFY(raw.value("group").toMap().contains("rogue"));
        QVERIFY(raw.value("map").toMap().value("opaque").toMap().contains("extra"));
    }
};
QTEST_GUILESS_MAIN(UnknownDataTest)
#include "test_unknown_data.moc"
