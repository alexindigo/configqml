// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configdraft.h"
#include "configjsoncodec.h"
#include "configreadprojection.h"
#include "configreadprojection_p.h"
#include "configpendingreply_p.h"
#include "confighandle_p.h"
#include "configstorehandle.h"
#include "storeprobe.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QProcess>
#include <QSemaphore>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include "eventwait.h"
#include <cstdio>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;

class GenericPrimitivesTest : public QObject {
    Q_OBJECT
private slots:
    void sharedRequestWaitsForActualOutcome() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath("user.json");
        compat::put(file, "{\"n\":0,\"opaque\":\"kept\"}");
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"n", D::leaf(T::SafeInteger, 0)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        options.jsonFiles = {{"user", "user", file}};
        const auto armed = std::make_shared<std::atomic_bool>(false);
        const auto held = std::make_shared<QSemaphore>();
        const auto release = std::make_shared<QSemaphore>();
        options.fileFault = [armed, held, release, file](const QString &phase,
                                                         const QString &path) {
            if (phase == "file-sync" && path == file && armed->exchange(false)) {
                qInfo("REQUEST_OUTCOME_HELD actual-file-sync");
                held->release();
                release->acquire();
            }
            return QString();
        };
        ConfigStore store(options);
        QVERIFY(test::awaitEvent(&store, &ConfigStore::statusChanged,
                                 [&] { return store.status() != ConfigStore::Status::Loading; }));
        QCOMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(store.setUser({"n"}, 1).accepted);
        QSignalSpy finished(&store, &ConfigStore::completed);
        armed->store(true);
        // Deliberately exceed the old fixture timer while its real fsync stays
        // held. This is a controlled release, not a new completion deadline.
        QTimer::singleShot(11500, &store, [held, release] {
            if (!held->tryAcquire())
                qFatal("request outcome fixture did not hold actual sync");
            release->release();
        });
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(saved.publicationState, QString("known"));
        int matches = 0;
        for (const auto &row : finished)
            matches += row.first().value<ConfigResult>().requestId == saved.requestId;
        QCOMPARE(matches, 1);
        QCOMPARE(QJsonDocument::fromJson(compat::bytes(file)).object(),
                 (QJsonObject{{"n", 1}, {"opaque", "kept"}}));
        options.fileFault = {};
        ConfigStore cold(options);
        QVERIFY(test::awaitEvent(&cold, &ConfigStore::statusChanged,
                                 [&] { return cold.status() != ConfigStore::Status::Loading; }));
        QCOMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(cold.value({"n"}).toInt(), 1);
        qInfo("REQUEST_OUTCOME_COMPLETE saved=1 receipts=1 cold=1 opaque=kept");
    }
    void selectedCandidateFactsMatchLayerOracle() {
        auto absent = D::leaf(T::String, QVariant());
        absent.optional = true;
        absent.defaultValue.reset();
        auto nullable = D::leaf(T::String, QVariant::fromValue(nullptr));
        nullable.nullable = true;
        const QString opaque = QString("key") + QChar(0x1e) + QChar(0x1f) + "/.__proto__";
        const ConfigSchema schema(
            D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 1)},
                                      {"absent", absent},
                                      {"null", nullable},
                                      {opaque, D::leaf(T::Boolean, false)}})},
                      {"b", D::group({{"count", D::leaf(T::SafeInteger, 2)}})}}));
        int comparisons = 0, cases = 0;
        for (auto policy :
             {ConfigAdmissionPolicy::FieldFallback, ConfigAdmissionPolicy::RetainLastGood,
              ConfigAdmissionPolicy::RejectInput})
            for (int flags = 0; flags < 64; ++flags) {
                ConfigSource user{
                    "user",
                    {"a"},
                    flags & 1 ? QVariantMap{}
                              : QVariantMap{{"count", 4}, {opaque, true}, {"unknown", "kept"}}};
                user.exists = !(flags & 2);
                user.available = !(flags & 4);
                ConfigSource legacy{"legacy", {"a"}, {{"count", 7}}};
                legacy.fallbackFor = "user";
                ConfigSource runtime{
                    "runtime",
                    {},
                    flags & 8
                        ? QVariantMap{}
                        : QVariantMap{{"a", QVariantMap{{"count", 9}, {"absent", "present"}}}}};
                const QList<ConfigLayer> layers{{"legacy",
                                                 ConfigLayerKind::File,
                                                 ConfigAdmissionPolicy::FieldFallback,
                                                 {legacy}},
                                                {"user",
                                                 ConfigLayerKind::File,
                                                 policy,
                                                 {user, {"other", {"b"}, {{"count", "invalid"}}}},
                                                 !(flags & 16)},
                                                {"environment",
                                                 ConfigLayerKind::Environment,
                                                 ConfigAdmissionPolicy::FieldFallback,
                                                 {{"environment", {"b"}, {{"count", 8}}}}},
                                                {"runtime",
                                                 ConfigLayerKind::Runtime,
                                                 ConfigAdmissionPolicy::FieldFallback,
                                                 {runtime}}};
                ConfigLayers base(schema, layers);
                QVERIFY2(base.valid(), qPrintable(base.errors().value(0).code));
                if (flags & 32) {
                    auto rejected = user;
                    rejected.raw["count"] = "invalid";
                    base.replaceSource("user", rejected);
                }
                for (auto target :
                     {ConfigSelection::Target::UserLayer, ConfigSelection::Target::RecordSlot,
                      ConfigSelection::Target::RuntimeLayer})
                    for (bool narrow : {false, true})
                        for (bool edited : {false, true}) {
                            const auto selection = ConfigSelection::capture(
                                schema,
                                narrow ? QList<ConfigPath>{{"a", "count"}} : QList<ConfigPath>{{}},
                                schema.leaves(), target, "selection");
                            const auto edits =
                                edited ? QMap<ConfigPath, std::optional<QVariant>>{{{"a", "count"},
                                                                                    std::nullopt}}
                                       : QMap<ConfigPath, std::optional<QVariant>>{};
                            const auto candidate = resolveCandidate(
                                base, ConfigEditBatch::capture(selection, edits), {}, {});
                            ConfigLayers oracle(base);
                            const bool accepted =
                                !edited || (target == ConfigSelection::Target::RuntimeLayer
                                                ? oracle.clearRuntime({"a", "count"}).accepted
                                                : oracle.applyEdits(edits).accepted);
                            ++cases;
                            QCOMPARE(candidate.valid(), accepted);
                            if (!accepted)
                                continue;
                            const auto effective = oracle.effective();
                            QCOMPARE(candidate.facts.size(), selection.leaves.size());
                            for (const auto &path : selection.leaves) {
                                const auto fact = candidate.facts.value(path);
                                QCOMPARE(fact.effective,
                                         valueAt(effective, path).value_or(QVariant()));
                                QCOMPARE(fact.maskedBy,
                                         target == ConfigSelection::Target::RuntimeLayer
                                             ? QString()
                                             : oracle.info(path).maskedBy);
                                std::optional<ConfigSource> source;
                                for (const auto &layer : layers)
                                    if (target == ConfigSelection::Target::RuntimeLayer
                                            ? layer.kind == ConfigLayerKind::Runtime
                                            : layer.writable) {
                                        for (const auto &entry : layer.sources)
                                            if (startsWith(path, entry.prefix)) {
                                                source = base.source(layer.id, entry.id);
                                                break;
                                            }
                                        break;
                                    }
                                QCOMPARE(fact.targetPresent,
                                         source && bool(valueAt(source->raw,
                                                                path.mid(source->prefix.size()))));
                                QCOMPARE(fact.edited, edits.contains(path));
                                ++comparisons;
                            }
                        }
            }
        QCOMPARE(cases, 2304);
        qInfo() << "SELECTED_FACT_ORACLE comparisons=" << comparisons;
    }
    void selectionAncestorIndexPreservesValidation() {
        const QString opaque = QString("field") + QChar(0x1e) + QChar(0x1f) + "/.__proto__";
        auto record = D::leaf(T::Record, QVariantMap{{"inside", 1}});
        record.fields = {{"inside", D::leaf(T::SafeInteger, 1)}};
        const ConfigSchema schema(
            D::group({{"a", D::group({{"b", D::leaf(T::SafeInteger, 1)},
                                      {opaque, D::leaf(T::String, QString())}})},
                      {"atomic", record},
                      {"outside", D::leaf(T::Boolean, false)}}));
        // Retain the previous validator as a finite acceptance oracle, including
        // invalid selections' partial facts, canonical ordering and both options.
        const auto previous = [&](const QList<ConfigPath> &paths, const QList<ConfigPath> &allowed,
                                  bool emptyMeansAll, bool rejectDuplicates) {
            QPair<QString, QList<ConfigPath>> result;
            QSet<ConfigPath> seen, included;
            if (!schema.valid() || (paths.isEmpty() && !emptyMeansAll))
                result.first = "selection";
            for (const auto &query : paths) {
                if (!schema.descriptor(query) || (rejectDuplicates && seen.contains(query))) {
                    result.first = "selection";
                    break;
                }
                seen.insert(ownPath(query));
                bool found = false;
                for (const auto &leaf : allowed)
                    if (startsWith(leaf, query)) {
                        included.insert(leaf);
                        found = true;
                    }
                if (!found)
                    result.first = "selection";
            }
            for (const auto &leaf : schema.leaves())
                if (allowed.contains(leaf) &&
                    ((paths.isEmpty() && emptyMeansAll) || included.contains(leaf)))
                    result.second.append(leaf);
            if (result.second.isEmpty())
                result.first = "selection";
            return result;
        };
        const QList<ConfigPath> queries{{},
                                        {"a"},
                                        {"a", "b"},
                                        {"a", opaque},
                                        {"atomic"},
                                        {"atomic", "inside"},
                                        {"outside"},
                                        {"unknown"},
                                        {"a", "missing"}};
        QList<QList<ConfigPath>> selections{{}};
        for (const auto &a : queries) {
            selections.append(QList<ConfigPath>{a});
            for (const auto &b : queries)
                selections.append(QList<ConfigPath>{a, b});
        }
        const auto leaves = schema.leaves();
        for (int mask = 0; mask < (1 << leaves.size()); ++mask) {
            QList<ConfigPath> allowed;
            for (qsizetype i = leaves.size(); i > 0; --i)
                if (mask & (1 << (i - 1)))
                    allowed.append(leaves[i - 1]);
            for (bool emptyMeansAll : {false, true})
                for (bool rejectDuplicates : {false, true})
                    for (const auto &paths : selections) {
                        const auto expected =
                            previous(paths, allowed, emptyMeansAll, rejectDuplicates);
                        const auto actual = ConfigSelection::capture(
                            schema, paths, allowed, ConfigSelection::Target::UserLayer, "selection",
                            emptyMeansAll, rejectDuplicates);
                        QCOMPARE(actual.error, expected.first);
                        QCOMPARE(actual.leaves, expected.second);
                        QVERIFY(actual.matchesSchema(schema));
                    }
        }
    }
    void selectedProjectionPreflightsOnlyItsSources() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto aFile = directory.filePath("a.json"), bFile = directory.filePath("b.json");
        compat::put(aFile, "{\"count\":3}");
        compat::put(bFile, "{\"count\":9}");
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 1)}})},
                                   {"b", D::group({{"count", D::leaf(T::SafeInteger, 2)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"a", {"a"}, {}}, {"b", {"b"}, {}}},
                           true}};
        options.jsonFiles = {{"user", "a", aFile}, {"user", "b", bFile}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QObject presentation;
        const auto identity = ConfigHandleSelection::capture(
            ConfigSelection::capture(options.schema, {{"a"}}, options.schema.leaves(),
                                     ConfigSelection::Target::ReadOnly, "selection"),
            {"a"});
        auto *selected =
            ConfigReadProjectionState::select(store.readProjection(), identity, &presentation);
        QVERIFY(selected);
        QCOMPARE(selected->snapshot().paths(), QList<ConfigPath>{{"count"}});
        QCOMPARE(selected->selectedState({"count"}).value().toInt(), 3);
        QVERIFY(!selected->selectedState({"b", "count"}).valid());
        const auto outside = ConfigHandleSelection::capture(
            ConfigSelection::capture(options.schema, {{"b"}}, options.schema.leaves(),
                                     ConfigSelection::Target::ReadOnly, "selection"),
            {"b"});
        QVERIFY(!ConfigReadProjectionState::select(selected, outside, &presentation));
        const ConfigSchema foreign(*options.schema.descriptor({}));
        const auto foreignIdentity = ConfigHandleSelection::capture(
            ConfigSelection::capture(foreign, {{"a"}}, foreign.leaves(),
                                     ConfigSelection::Target::ReadOnly, "selection"),
            {"a"});
        QVERIFY(!ConfigReadProjectionState::select(store.readProjection(), foreignIdentity,
                                                   &presentation));
        compat::put(bFile, "{");
        std::unique_ptr<ConfigPendingReply> read(ConfigPendingReplyState::read(selected));
        QTRY_VERIFY(read->isFinished());
        QVERIFY2(!read->isError(), qPrintable(read->error().code()));
        QCOMPARE(read->value().snapshot().selectedState({"count"}).value().toInt(), 3);
        std::unique_ptr<ConfigPendingReply> root(
            ConfigPendingReplyState::read(store.readProjection()));
        QTRY_VERIFY(root->isFinished());
        QVERIFY(root->isError());
        QCOMPARE(root->error().code(), QString("read-acquisition"));
        compat::put(bFile, "{\"count\":9}");
        QVERIFY(store.setUser({"a", "count"}, 7).accepted);
        QTRY_COMPARE(selected->selectedState({"count"}).value().toInt(), 7);
        QCOMPARE(selected->snapshot().paths(), QList<ConfigPath>{{"count"}});
        QCOMPARE(compat::bytes(aFile), QByteArray("{\"count\":3}"));
    }
    void selectedProjectionTracksDraftCandidates() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"group", D::group({{"a", D::leaf(T::SafeInteger, 1)},
                                                       {"b", D::leaf(T::SafeInteger, 2)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"group", QVariantMap{{"a", 4}, {"b", 5}}}}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"group"}});
        QVERIFY(draft->valid());
        QObject presentation;
        const auto identity = ConfigHandleSelection::capture(
            ConfigSelection::capture(options.schema, {{"group", "a"}}, draft->paths(),
                                     ConfigSelection::Target::UserLayer, "selection"),
            {"group"});
        auto *frozen =
            ConfigReadProjectionState::select(draft->readProjection(), identity, &presentation);
        auto *preview =
            ConfigReadProjectionState::select(draft->previewProjection(), identity, &presentation);
        QVERIFY(frozen && preview);
        QVERIFY(draft->setValue({"group", "a"}, 8).accepted);
        QCOMPARE(frozen->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Set);
        QCOMPARE(frozen->selectedState({"a"}).authoredValue().toInt(), 8);
        QVERIFY(!frozen->selectedState({"a"}).candidateAvailable());
        QTRY_VERIFY(frozen->selectedState({"a"}).candidateAvailable());
        QTRY_COMPARE(preview->selectedState({"a"}).candidateValue().toInt(), 8);
        QCOMPARE(frozen->snapshot().paths(), QList<ConfigPath>{{"a"}});
        QCOMPARE(preview->snapshot().paths(), QList<ConfigPath>{{"a"}});
        QVERIFY(!preview->selectedState({"b"}).valid());
        bool staged = false;
        draft->stageReset({{"group", "a"}}, this, [&](const ConfigResult &result) {
            QCOMPARE(result.status, QString("staged"));
            staged = true;
        });
        QTRY_VERIFY(staged);
        QCOMPARE(frozen->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Remove);
        QCOMPARE(frozen->selectedState({"a"}).candidateValue().toInt(), 1);
        QCOMPARE(preview->selectedState({"a"}).candidateValue().toInt(), 1);
        std::unique_ptr<ConfigPendingReply> refused(ConfigPendingReplyState::read(frozen));
        QTRY_VERIFY(refused->isFinished());
        QCOMPARE(refused->error().code(), QString("read-frozen"));
        QVERIFY(draft->discard());
        QTRY_COMPARE(frozen->selectedState({"a"}).pending(), ConfigSelectedState::Pending::None);
        QCOMPARE(frozen->selectedState({"a"}).value().toInt(), 4);
        std::unique_ptr<ConfigPendingReply> closed(ConfigPendingReplyState::read(preview));
        QTRY_VERIFY(closed->isFinished());
        QVERIFY(closed->isError());
        QCOMPARE(closed->error().code(), QString("read-owner"));
        QCOMPARE(store.value({"group", "a"}).toInt(), 4);
        QVERIFY(store.inputs().isEmpty());
    }
    void selectedProjectionParentLifetime() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *scope = store.scope({"a"}, false);
        const auto identity = ConfigHandleSelection::capture(
            ConfigSelection::capture(options.schema, {{"a", "count"}}, options.schema.leaves(),
                                     ConfigSelection::Target::ReadOnly, "selection"),
            {"a"});
        QObject presentation, scheduler;
        auto *selected =
            ConfigReadProjectionState::select(scope->readProjection(), identity, &presentation);
        QVERIFY(selected);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        std::unique_ptr<ConfigPendingReply> reply(ConfigPendingReplyState::read(selected));
        QTRY_COMPARE(held.size(), 1);
        scope->detach();
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        ConfigStoreTestAccess::releaseReader(store, held);
        QVERIFY(!ConfigReadProjectionState::available(selected));
        QCOMPARE(selected->snapshot().paths(), QList<ConfigPath>{{"count"}});
    }
    void generatedSelectionUsesDeclarations() {
        auto absent = D(T::String);
        absent.optional = true;
        auto record = D::leaf(T::Record, QVariantMap{{"inside", 1}});
        record.fields = {{"inside", D::leaf(T::SafeInteger, 0)}};
        // This schema represents a plugin unknown at library build time. Lookup
        // must use declarations even when no value/default has ever existed.
        ConfigSchema schema(D::group(
            {{"plugin.with/slashes", D::group({{"absent", absent},
                                               {"read", D::leaf(T::Boolean, false)},
                                               {"__proto__", record},
                                               {"constructor", D::leaf(T::String, QString())}})},
             {"outside", D::leaf(T::SafeInteger, 0)}}));
        auto selected = ConfigSelection::capture(schema, {{}}, schema.leaves(),
                                                 ConfigSelection::Target::UserLayer, "selection");
        selected.lifetime = std::make_shared<std::atomic_bool>(true);
        auto root = ConfigHandleSelection::capture(selected);
        QVERIFY(root.valid());
        QCOMPARE(root.scopes(), QStringList({"plugin.with/slashes"}));
        QCOMPARE(root.fields(), QStringList({"outside"}));
        const auto plugin = root.scope({"plugin.with/slashes"});
        QVERIFY(plugin.valid());
        QCOMPARE(plugin.fields(), QStringList({"__proto__", "absent", "constructor", "read"}));
        QVERIFY(plugin.scopes().isEmpty());
        const auto field = plugin.scope({"absent"});
        QVERIFY(field.valid());
        QCOMPARE(field.selection().target, selected.target);
        QVERIFY(field.selection().matchesSchema(schema));
        QCOMPARE(field.selection().lifetime, selected.lifetime);
        QVERIFY(field.descriptor()->optional);
        QVERIFY(!field.descriptor()->defaultValue);
        QVERIFY(field.fields().isEmpty());
        QVERIFY(field.scopes().isEmpty());
        QVERIFY(!plugin.scope({"__proto__", "inside"}).valid());
        QVERIFY(!root.scope({"plugin", "with/slashes"}).valid());
        QVERIFY(!root.scope({"unknown"}).valid());
        // Mutable copies cannot rewrite retained schema, capability or key data.
        auto copied = *plugin.descriptor();
        copied.fields.clear();
        QCOMPARE(plugin.fields().size(), 4);
        auto widened = field.selection();
        widened.target = ConfigSelection::Target::RuntimeLayer;
        QCOMPARE(field.selection().target, ConfigSelection::Target::UserLayer);
    }
    void generatedSelectionNeverWidens() {
        ConfigSchema schema(D::group({{"a", D::group({{"first", D::leaf(T::SafeInteger, 1)},
                                                      {"second", D::leaf(T::SafeInteger, 2)}})},
                                      {"b", D::group({{"third", D::leaf(T::SafeInteger, 3)}})}}));
        auto selected = ConfigSelection::capture(schema, {{"a", "first"}, {"b"}}, schema.leaves(),
                                                 ConfigSelection::Target::ReadOnly, "selection");
        auto root = ConfigHandleSelection::capture(selected);
        const auto a = root.scope({"a"});
        QVERIFY(a.valid());
        QCOMPARE(a.fields(), QStringList({"first"}));
        QCOMPARE(a.selection().leaves, QList<ConfigPath>({{"a", "first"}}));
        QCOMPARE(a.selection().target, ConfigSelection::Target::ReadOnly);
        QVERIFY(!a.scope({"second"}).valid());
        QVERIFY(!a.scope({"b"}).valid());
        const auto unionSelection = root.select({{"a"}, {"b", "third"}});
        QVERIFY(unionSelection.valid());
        QCOMPARE(unionSelection.selection().leaves, selected.leaves);
        QCOMPARE(unionSelection.descriptor()->fields.value("a").fields.keys(),
                 QStringList({"first"}));
        QVERIFY(!root.select({}).valid());
        QVERIFY(!root.select({{"a"}, {"a"}}).valid());
        QVERIFY(!root.select({{"a", "second"}}).valid());
        QVERIFY(!root.select({{"missing"}}).scope({}).valid());
        auto bad = selected;
        bad.leaves.append({"a", "first", "member"});
        QVERIFY(!ConfigHandleSelection::capture(bad).valid());
        auto replacement =
            ConfigSelection::capture(ConfigSchema(*schema.descriptor({})), {{}}, schema.leaves(),
                                     ConfigSelection::Target::ReadOnly, "selection");
        replacement.schemaIdentity = selected.schemaIdentity;
        QCOMPARE(ConfigHandleSelection::capture(replacement).error(), QString("handle-schema"));
    }
    void generatedSelectionOwnsBorrowedPaths() {
        ConfigSchema schema(
            D::group({{"group", D::group({{"field", D::leaf(T::String, QString())}})}}));
        auto selection = ConfigSelection::capture(
            schema, {{}}, schema.leaves(), ConfigSelection::Target::RuntimeLayer, "selection");
        auto root = ConfigHandleSelection::capture(selection);
        QChar groupStorage[] = {QChar('g'), QChar('r'), QChar('o'), QChar('u'), QChar('p')};
        const auto group = QString::fromRawData(groupStorage, 5);
        auto scoped = root.scope({group});
        groupStorage[0] = QChar('x');
        QCOMPARE(scoped.prefix(), ConfigPath({"group"}));
        QCOMPARE(scoped.fields(), QStringList({"field"}));
        QCOMPARE(scoped.selection().target, ConfigSelection::Target::RuntimeLayer);
    }
    void pendingRefusalIsQueued() {
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::refuse("unsupported-operation", "selection"));
        QSignalSpy finished(reply.get(), &ConfigPendingReply::finished);
        QVERIFY(!reply->isFinished());
        QVERIFY(finished.isEmpty());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        const auto error = reply->error();
        QCOMPARE(error.code(), QString("unsupported-operation"));
        QCOMPARE(error.phase(), QString("selection"));
        QCOMPARE(error.message(), QString("configqml: unsupported-operation"));
        reply.reset();
        QCOMPARE(error.code(), QString("unsupported-operation"));
    }
    void pendingReadComposesExactViews_data() {
        QTest::addColumn<bool>("newerInput");
        QTest::newRow("unchanged") << false;
        QTest::newRow("newer-input") << true;
    }
    void pendingReadComposesExactViews() {
        QFETCH(bool, newerInput);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})},
                                   {"b", D::group({{"count", D::leaf(T::SafeInteger, 9)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *origin = store.scope({"a"}, false)->readProjection();
        auto *first = store.readProjection();
        auto *second = store.scope({"a"}, false)->readProjection();
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        const auto inputs = store.inputs().size();
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::read(origin, {}, {first, second}));
        QSignalSpy finished(reply.get(), &ConfigPendingReply::finished);
        QVERIFY(!reply->isFinished());
        QTRY_COMPARE(held.size(), 1);
        QVERIFY(finished.isEmpty());
        ConfigStoreTestAccess::releaseReader(store, held); // Only the initiating cut.
        QTRY_COMPARE(held.size(), 2);
        QVERIFY(finished.isEmpty()); // Neither requested exact view has installed its capture.
        QCOMPARE(store.inputs().size(), inputs);
        if (newerInput)
            QVERIFY(store.setUser({"a", "count"}, 7).accepted);
        // Release the second requested view first: the reply still needs the first.
        const auto firstCapture = held.takeFirst();
        ConfigStoreTestAccess::releaseReader(store, held);
        QCoreApplication::processEvents();
        QVERIFY(!reply->isFinished());
        ConfigStoreTestAccess::inject(store, firstCapture);
        QVERIFY(!reply->isFinished()); // Terminal delivery is queued, including a no-op.
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!reply->isError());
        const auto captured = reply->value();
        QVERIFY(captured.generation().valid());
        QCOMPARE(captured.snapshot().selectedState({"count"}).value().toInt(), 3);
        const auto views = captured.observations();
        QCOMPARE(views.size(), 2);
        QCOMPARE(views[0].status(), QString("observed"));
        QCOMPARE(views[1].status(), QString("observed"));
        QCOMPARE(views[0].snapshot().selectedState({"b", "count"}).value().toInt(), 9);
        QVERIFY(!views[1].snapshot().selectedState({"b", "count"}).valid());
        QCOMPARE(views[1].snapshot().selectedState({"count"}).value().toInt(), 3);
        const bool rootCoversNewer =
            views[0].snapshot().selectedState({"a", "count"}).value().toInt() == 7;
        const QList<ConfigPath> expectedPending = newerInput && !rootCoversNewer
                                                      ? QList<ConfigPath>{{"a", "count"}}
                                                      : QList<ConfigPath>{};
        QCOMPARE(views[0].pending(), expectedPending);
        QCOMPARE(views[1].pending(),
                 newerInput ? QList<ConfigPath>{{"count"}} : QList<ConfigPath>{});
        QVERIFY(!captured.write()); // Observation is not a fabricated publication receipt.
        QProcess consumer;
        consumer.start(
            QCoreApplication::applicationFilePath(),
            {"--inert-daemon",
             QString::number(captured.snapshot().selectedState({"count"}).value().toInt())});
        QVERIFY(consumer.waitForFinished(10000));
        QCOMPARE(consumer.exitCode(), 0);
        QCOMPARE(consumer.readAllStandardOutput().trimmed(), QByteArray("DAEMON_ARGV 3"));
        // Late duplicate captures cannot finish again or mutate detached results.
        ConfigStoreTestAccess::inject(store, firstCapture);
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 1);
        reply.reset();
        QCOMPARE(captured.snapshot().selectedState({"count"}).value().toInt(), 3);
    }
    void pendingViewSelectionRefusesBeforeCapture() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})},
                                   {"b", D::group({{"count", D::leaf(T::SafeInteger, 9)}})}}));
        ConfigStore store(options), unrelated(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(unrelated.status(), ConfigStore::Status::Ready);
        auto *root = store.readProjection();
        auto *narrow = store.scope({"a"}, false)->readProjection();
        QSignalSpy reads(root, &ConfigReadProjection::completed);
        for (auto *view : {narrow, unrelated.readProjection()}) {
            std::unique_ptr<ConfigPendingReply> reply(
                ConfigPendingReplyState::read(root, {}, {view}));
            QVERIFY(!reply->isFinished());
            QTRY_VERIFY(reply->isFinished());
            QVERIFY(reply->isError());
            QCOMPARE(reply->error().code(), QString("read-observation"));
            QCOMPARE(reply->error().phase(), QString("selection"));
            QCOMPARE(reply->value().observations().size(), 1);
            QCOMPARE(reply->value().observations()[0].status(), QString("not_observed"));
        }
        QVERIFY(reads.isEmpty());
        QVERIFY(store.inputs().isEmpty());
    }
    void pendingReaderFailureRetainsInitiatingCapture() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath("pending.json");
        const QByteArray bytes("{\"a\":{\"count\":3}}");
        compat::put(file, bytes);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 1)}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        options.jsonFiles = {{"user", "user", file}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *origin = store.scope({"a"}, false)->readProjection();
        auto *reader = store.readProjection();
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::read(origin, {}, {reader}));
        QTRY_COMPARE(held.size(), 1);
        const auto lastGood = reader->observation();
        compat::put(file, "{");
        ConfigStoreTestAccess::releaseReader(store, held);
        QTRY_COMPARE(held.size(), 1);
        QVERIFY(!reply->isFinished());
        ConfigStoreTestAccess::releaseReader(store, held);
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        QCOMPARE(reply->error().phase(), QString("observation"));
        QCOMPARE(reply->error().code(), QString("read-acquisition"));
        const auto captured = reply->value();
        QVERIFY(captured.generation().valid());
        QCOMPARE(captured.snapshot().selectedState({"count"}).value().toInt(), 3);
        QVERIFY(!captured.write());
        QCOMPARE(reader->observation(), lastGood);
        QCOMPARE(captured.observations()[0].status(), QString("failed"));
        compat::put(file, bytes);
        std::unique_ptr<ConfigPendingReply> recovery(
            ConfigPendingReplyState::read(reader, captured.generation()));
        QTRY_COMPARE(held.size(), 1);
        ConfigStoreTestAccess::releaseReader(store, held);
        QTRY_VERIFY(recovery->isFinished());
        QVERIFY(!recovery->isError());
        QCOMPARE(recovery->value().snapshot().selectedState({"a", "count"}).value().toInt(), 3);
        QVERIFY(store.inputs().isEmpty());
        QCOMPARE(compat::bytes(file), bytes);
    }
    void pendingReplySurvivesPresentationLoss() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{"a", D::group({{"count", D::leaf(T::SafeInteger, 3)}})}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *scope = store.scope({"a"}, false);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        std::unique_ptr<ConfigPendingReply> reply(
            ConfigPendingReplyState::read(scope->readProjection()));
        QTRY_COMPARE(held.size(), 1);
        delete scope;
        QVERIFY(!reply->isFinished());
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(reply->isError());
        QCOMPARE(reply->error().phase(), QString("lifetime"));
        ConfigStoreTestAccess::releaseReader(store, held);
        // Consumer deletion during the one terminal signal has no continuation.
        auto *destroyed = ConfigPendingReplyState::refuse("unsupported-operation", "selection");
        QPointer<ConfigPendingReply> guard(destroyed);
        connect(destroyed, &ConfigPendingReply::finished, this, [destroyed] { delete destroyed; });
        QTRY_VERIFY(!guard);
    }
    void pendingReadNotificationFence() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        bool started = false, inside = false, early = false;
        std::unique_ptr<ConfigPendingReply> reply;
        connect(&store, &ConfigStore::valuesChanged, this, [&] {
            if (started)
                return;
            started = inside = true;
            reply.reset(ConfigPendingReplyState::read(store.readProjection()));
            connect(reply.get(), &ConfigPendingReply::finished, this, [&] { early |= inside; });
            QEventLoop nested;
            ConfigStoreTestAccess::run(
                store, [](ConfigWorker &) {}, &nested, [&] { nested.quit(); });
            nested.exec();
            early |= reply->isFinished();
            inside = false;
        });
        QVERIFY(store.setUser({"count"}, 3).accepted);
        QTRY_VERIFY(reply && reply->isFinished());
        QVERIFY(!early);
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().snapshot().selectedState({"count"}).value().toInt(), 3);
    }
    void explicitCandidateTargets_data() {
        QTest::addColumn<bool>("runtime");
        QTest::newRow("user") << false;
        QTest::newRow("runtime") << true;
    }
    void explicitCandidateTargets() {
        QFETCH(bool, runtime);
        const ConfigSchema schema(D::group({{"a", D::leaf(T::SafeInteger, 1)},
                                            {"b", D::leaf(T::SafeInteger, 2)},
                                            {"c", D::leaf(T::SafeInteger, 3)}}));
        ConfigLayers base(
            schema, {{"user",
                      ConfigLayerKind::File,
                      ConfigAdmissionPolicy::FieldFallback,
                      {{"user", {}, {{"a", 4}, {"b", 5}, {"unknown", "user-owned"}}}},
                      true},
                     {"runtime",
                      ConfigLayerKind::Runtime,
                      ConfigAdmissionPolicy::FieldFallback,
                      {{"runtime", {}, {{"a", 8}, {"c", 9}, {"unknown", "runtime-owned"}}}}}});
        const auto target =
            runtime ? ConfigSelection::Target::RuntimeLayer : ConfigSelection::Target::UserLayer;
        const auto selection =
            ConfigSelection::capture(schema, {{}}, schema.leaves(), target, "edit-selection");
        auto batch = ConfigEditBatch::capture(selection, {{{"a"}, std::nullopt}, {{"b"}, 6}});
        batch.sequence = 17;
        QList<ConfigInput> admitted;
        const auto candidate = resolveCandidate(base, batch, {}, {}, [&](const ConfigInput &input) {
            admitted.append(input);
            return QString();
        });
        QVERIFY2(candidate.valid(), qPrintable(candidate.error));
        QCOMPARE(admitted.size(), 2);
        QCOMPARE(admitted[0].kind,
                 runtime ? ConfigInput::Kind::ClearRuntime : ConfigInput::Kind::Reset);
        QCOMPARE(admitted[1].kind, runtime ? ConfigInput::Kind::Runtime : ConfigInput::Kind::User);
        QCOMPARE(admitted[0].sequence, quint64(17));
        const auto a = candidate.facts.value({"a"}), b = candidate.facts.value({"b"});
        QVERIFY(a.targetPresent);
        QCOMPARE(b.targetPresent, !runtime);
        QCOMPARE(a.effective.toInt(), runtime ? 4 : 8);
        QCOMPARE(b.effective.toInt(), 6);
        QCOMPARE(a.maskedBy, runtime ? QString() : QString("runtime"));
        QCOMPARE(candidate.facts.value({"c"}).targetPresent, runtime);
        QVERIFY(a.edited && b.edited);
        QVERIFY(!candidate.facts.value({"c"}).edited);
        const QString targetId = runtime ? "runtime" : "user";
        auto expected = base.source(targetId, targetId)->raw;
        expected.remove("a");
        expected["b"] = qint64(6);
        QCOMPARE(candidate.layers->source(targetId, targetId)->raw, expected);
        const QString otherId = runtime ? "user" : "runtime";
        QCOMPARE(candidate.layers->source(otherId, otherId)->raw,
                 base.source(otherId, otherId)->raw);
        QCOMPARE(base.value({"a"})->toInt(), 8);
        QCOMPARE(base.value({"b"})->toInt(), 5);
        QCOMPARE(base.info({"a"}).intentRevision, quint64(0));
        QVERIFY(base.takeProjectionEffects().isEmpty());
    }
    void runtimeCandidateRefusalIsAtomic() {
        const ConfigSchema schema(
            D::group({{"a", D::leaf(T::SafeInteger, 1)}, {"b", D::leaf(T::SafeInteger, 2)}}));
        ConfigSource source{"runtime", {}, {{"a", 4}, {"b", 5}}};
        source.unknownData = ConfigUnknownData::Strict;
        ConfigLayers base(schema, {{"runtime",
                                    ConfigLayerKind::Runtime,
                                    ConfigAdmissionPolicy::FieldFallback,
                                    {source}}});
        const auto selection = ConfigSelection::capture(
            schema, {{}}, schema.leaves(), ConfigSelection::Target::RuntimeLayer, "edit-selection");
        const auto batch = ConfigEditBatch::capture(selection, {{{"a"}, 6}, {{"b"}, 7}});
        int checked = 0;
        const auto refused = resolveCandidate(base, batch, {}, {}, [&](const ConfigInput &input) {
            ++checked;
            return input.path == ConfigPath{"b"} ? QString("policy-refused") : QString();
        });
        QCOMPARE(checked, 2);
        QCOMPARE(refused.error, QString("policy-refused"));
        QVERIFY(!refused.layers);
        QCOMPARE(base.source("runtime", "runtime")->raw, source.raw);
        QCOMPARE(base.info({"a"}).intentRevision, quint64(0));
        const auto invalid =
            ConfigEditBatch::capture(selection, {{{"a"}, 6}, {{"b"}, "wrong-type"}});
        QVERIFY(!invalid.error.isEmpty());
        checked = 0;
        QVERIFY(!resolveCandidate(base, invalid, {}, {}, [&](const ConfigInput &) {
                     ++checked;
                     return QString();
                 }).valid());
        QCOMPARE(checked, 0);
        const ConfigLayers missing(schema, {});
        QCOMPARE(resolveCandidate(missing, batch, {}, {}).error, QString("no-write-target"));
        const auto removed = ConfigEditBatch::capture(selection, {{{"a"}, std::nullopt}});
        QCOMPARE(resolveCandidate(missing, removed, {}, {}).error, QString("no-runtime-target"));
    }
    void candidateProjectsWholeBatch_data() {
        QTest::addColumn<bool>("runtime");
        QTest::addColumn<int>("b");
        QTest::newRow("user-complete-candidate") << false << 2;
        QTest::newRow("user-invalid-final-candidate") << false << 3;
        QTest::newRow("runtime-complete-candidate") << true << 2;
        QTest::newRow("runtime-keeps-file-projection-input") << true << 3;
    }
    void emptyCandidateUsesAdmittedState_data() {
        QTest::addColumn<int>("target");
        QTest::newRow("user") << int(ConfigSelection::Target::UserLayer);
        QTest::newRow("runtime") << int(ConfigSelection::Target::RuntimeLayer);
        QTest::newRow("record-slot") << int(ConfigSelection::Target::RecordSlot);
    }
    void emptyCandidateUsesAdmittedState() {
        QFETCH(int, target);
        const ConfigSchema schema(D::group(
            {{"count", D::leaf(T::SafeInteger, 1)}, {"other", D::leaf(T::SafeInteger, 2)}}));
        int projections = 0;
        ConfigProjectionSpec projection{
            "project", {"user"}, {}, [&](const ConfigProjectionInput &input) {
                ++projections;
                auto values = input.values;
                values["other"] = 7;
                return ConfigProjectionResult{values, input.compatibilityRaw, {}, {}};
            }};
        const QVariantMap raw{{"count", 4}, {"unknown", "preserved"}};
        ConfigLayers base(schema,
                          {{"user",
                            ConfigLayerKind::File,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"user", {}, raw}},
                            true},
                           {"runtime",
                            ConfigLayerKind::Runtime,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"runtime", {}, {{"count", 9}}}}}},
                          {projection});
        QVERIFY(base.valid());
        QCOMPARE(projections, 1);
        base.takeProjectionEffects();
        const auto before = base.effective();
        auto selection = ConfigSelection::capture(schema, {{}}, schema.leaves(),
                                                  ConfigSelection::Target(target), "selection");
        selection.lifetime = std::make_shared<std::atomic_bool>(true);
        projections = 0;
        int admissions = 0;
        const auto candidate = resolveCandidate(base, ConfigEditBatch::capture(selection, {}), {},
                                                {}, [&](const ConfigInput &) {
                                                    ++admissions;
                                                    return QString("unexpected-edit");
                                                });
        QVERIFY2(candidate.valid(), qPrintable(candidate.error));
        QCOMPARE(candidate.layers->effective(), before);
        QCOMPARE(candidate.facts.value({"count"}).effective.toInt(), 9);
        QCOMPARE(candidate.facts.value({"other"}).effective.toInt(), 7);
        QVERIFY(candidate.facts.value({"count"}).targetPresent);
        QVERIFY(!candidate.facts.value({"other"}).targetPresent);
        QVERIFY(!candidate.facts.value({"count"}).edited);
        QCOMPARE(candidate.facts.value({"count"}).maskedBy,
                 target == int(ConfigSelection::Target::RuntimeLayer) ? QString()
                                                                      : QString("runtime"));
        QCOMPARE(candidate.layers->source("user", "user")->raw, raw);
        QCOMPARE(candidate.layers->info({"count"}).intentRevision, quint64(0));
        QVERIFY(candidate.layers->takeProjectionEffects().isEmpty());
        QCOMPARE(admissions, 0);
        QCOMPARE(projections, 0); // A read of admitted facts is not a local transaction.
        *selection.lifetime = false;
        QCOMPARE(resolveCandidate(base, ConfigEditBatch::capture(selection, {}), {}, {}).error,
                 QString("edit-closed"));
        const ConfigSchema foreign(*schema.descriptor({}));
        selection.lifetime.reset();
        const ConfigLayers foreignBase(foreign, {});
        QCOMPARE(
            resolveCandidate(foreignBase, ConfigEditBatch::capture(selection, {}), {}, {}).error,
            QString("edit-schema"));
    }
    void candidateProjectsWholeBatch() {
        QFETCH(bool, runtime);
        QFETCH(int, b);
        auto integer = D::leaf(T::SafeInteger, 1);
        integer.admissionPolicy = D::AdmissionPolicy::Strict;
        const ConfigSchema schema(D::group({{"a", integer}, {"b", integer}}));
        int projections = 0;
        ConfigProjectionSpec projection{
            "pair", {"user"}, {}, [&](const ConfigProjectionInput &input) {
                ++projections;
                auto values = input.values;
                if (input.raw.value("a") != input.raw.value("b"))
                    values["a"] = false;
                return ConfigProjectionResult{values, input.compatibilityRaw, {}, {}};
            }};
        const QVariantMap raw{{"a", 1}, {"b", 1}};
        ConfigLayers base(schema,
                          {{"user",
                            ConfigLayerKind::File,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"user", {}, raw}},
                            true},
                           {"runtime",
                            ConfigLayerKind::Runtime,
                            ConfigAdmissionPolicy::FieldFallback,
                            {{"runtime", {}, raw}}}},
                          {projection});
        QVERIFY(base.valid());
        QVERIFY(!base.takeProjectionEffects().isEmpty()); // Initial source acquisition event.
        projections = 0;
        const auto selection = ConfigSelection::capture(
            schema, {{}}, schema.leaves(),
            runtime ? ConfigSelection::Target::RuntimeLayer : ConfigSelection::Target::UserLayer,
            "edit-selection");
        const auto candidate = resolveCandidate(
            base, ConfigEditBatch::capture(selection, {{{"a"}, 2}, {{"b"}, b}}), {}, {});
        QCOMPARE(projections, 1); // Never evaluate the inconsistent intermediate a=2,b=1.
        QCOMPARE(candidate.valid(), runtime || b == 2);
        if (candidate.valid()) {
            QCOMPARE(candidate.facts.value({"a"}).effective.toInt(), runtime ? 2 : 1);
            QCOMPARE(candidate.facts.value({"b"}).effective.toInt(), runtime ? b : 1);
        } else {
            QVERIFY(!candidate.error.isEmpty());
            QVERIFY(!candidate.layers);
        }
        QCOMPARE(base.source("runtime", "runtime")->raw, raw);
        QCOMPARE(base.source("user", "user")->raw, raw);
        QCOMPARE(base.info({"a"}).intentRevision, quint64(0));
        QVERIFY(base.takeProjectionEffects().isEmpty());
    }
    void mixedDraftPending() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"a", D::leaf(T::SafeInteger, 1)}, {"b", D::leaf(T::SafeInteger, 2)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"a", 4}, {"b", 5}}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        auto *projection = draft->readProjection();
        QCOMPARE(compat::request(store, [&] { return draft->stageReset({{}}); }).status,
                 QString("staged"));
        QCOMPARE(projection->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(draft->setValue({"a"}, 8).accepted);
        QCOMPARE(projection->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Set);
        QVERIFY(!projection->selectedState({"a"}).candidateAvailable());
        QVERIFY(!projection->selectedState({"b"}).candidateAvailable());
        QTRY_VERIFY(projection->selectedState({"a"}).candidateAvailable());
        QCOMPARE(projection->selectedState({"a"}).candidateValue().toInt(), 8);
        QCOMPARE(projection->selectedState({"b"}).candidateValue().toInt(), 2);
        QCOMPARE(projection->selectedState({"b"}).pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(store.inputs().isEmpty());
        QVERIFY(draft->discard());
        QCOMPARE(store.value({"a"}).toInt(), 4);
    }
    void closedPreviewRejectsDelayedCapture() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        QVERIFY(draft->setValue({"count"}, 7).accepted);
        auto *preview = draft->previewProjection();
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReadReplies(store, &scheduler, held, onHeld);
        bool finished = false;
        preview->synchronize({}, this, [&](const ConfigReadResult &result) {
            finished = true;
            QCOMPARE(result.status(), QString("failed"));
        });
        QEventLoop loop;
        onHeld = [&] { loop.quit(); };
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(10000);
        loop.exec();
        onHeld = {};
        QVERIFY(!held.isEmpty());
        QVERIFY(!finished);
        const auto captured = held.first().reads.first().snapshot;
        QCOMPARE(captured.selectedState({"count"}).value().toInt(), 7);
        QVERIFY(draft->discard());
        QVERIFY(finished);
        QCOMPARE(preview->snapshot().selectedState({"count"}).value().toInt(), 4);
        QVERIFY(!captured.observation().valid());
        ConfigStoreTestAccess::releaseReader(store, held);
        QCOMPARE(preview->snapshot().selectedState({"count"}).value().toInt(), 4);
        QCOMPARE(captured.selectedState({"count"}).value().toInt(), 7);
    }
    void notificationFence() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *projection = store.readProjection();
        bool started = false, inside = false, called = false, early = false;
        connect(&store, &ConfigStore::valuesChanged, this, [&] {
            if (started)
                return;
            started = inside = true;
            projection->synchronize({}, this, [&](const ConfigReadResult &result) {
                early |= inside;
                called = true;
                QCOMPARE(result.status(), QString("observed"));
                QCOMPARE(result.snapshot().selectedState({"count"}).value().toInt(), 3);
            });
            QEventLoop nested;
            ConfigStoreTestAccess::run(
                store, [](ConfigWorker &) {}, &nested, [&] { nested.quit(); });
            nested.exec(); // Reader's worker command finished during the notification pass.
            early |= called;
            inside = false;
        });
        QVERIFY(store.setUser({"count"}, 3).accepted);
        QTRY_VERIFY(called);
        QVERIFY(!early);
    }
    void ordinaryDraftCandidate() {
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 1)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"count", 4}}}},
                           true},
                          {"runtime",
                           ConfigLayerKind::Runtime,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"runtime", {}, {{"count", 9}}}},
                           false}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft();
        auto *projection = draft->readProjection(); // Provisional slice-facing capability.
        QSignalSpy completed(&store, &ConfigStore::completed);
        const auto inputs = store.inputs().size();
        QVERIFY(draft->setValue({"count"}, 7).accepted);
        QVERIFY(!draft->busy());
        QCOMPARE(projection->selectedState({"count"}).pending(), ConfigSelectedState::Pending::Set);
        QVERIFY(!projection->selectedState({"count"}).candidateAvailable());
        QVERIFY(draft->setValue({"count"}, 8).accepted);
        QTRY_VERIFY(projection->selectedState({"count"}).candidateAvailable());
        const auto captured = projection->snapshot();
        const auto state = captured.selectedState({"count"});
        QCOMPARE(state.authoredValue().toInt(), 8);
        QCOMPARE(state.value().toInt(), 9);
        QCOMPARE(state.candidateValue().toInt(), 9);
        QVERIFY(state.targetPresent());
        QCOMPARE(state.maskedBy(), QString("runtime"));
        QVERIFY(completed.isEmpty()); // Setters add no public operation completions.
        QCOMPARE(store.inputs().size(), inputs);
        QVERIFY(draft->setValue({"count"}, 6).accepted);
        QVERIFY(!draft->busy());
        const auto applied = compat::request(store, [&] { return draft->apply(); });
        QCOMPARE(applied.status, QString("saved")); // No new candidate-busy gate.
        QCOMPARE(captured.selectedState({"count"}).authoredValue().toInt(), 8);
        QCOMPARE(store.value({"count"}).toInt(), 9);
    }
    void selectedCancellableReset_data() {
        QTest::addColumn<QByteArray>("original");
        QTest::addColumn<int>("inheritedCount");
        QTest::addColumn<int>("inheritedHidden");
        QTest::newRow("override") << QByteArray("{\"count\":4,\"hidden\":5}") << 3 << 8;
        QTest::newRow("equal-override") << QByteArray("{\"count\":3,\"hidden\":8}") << 3 << 8;
        QTest::newRow("no-override") << QByteArray("{}") << 3 << 8;
        QTest::newRow("unknown-sibling")
            << QByteArray("{\"count\":4,\"hidden\":5,\"opaque\":18446744073709551615}") << 1 << 2;
        QTest::newRow("unknown-only") << QByteArray("{\"opaque\":true}") << 1 << 2;
    }
    void selectedCancellableReset() {
        QFETCH(QByteArray, original);
        QFETCH(int, inheritedCount);
        QFETCH(int, inheritedHidden);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath("configuration.json");
        const auto protectedFile = directory.filePath("content.json");
        compat::put(file, original);
        const QByteArray protectedBytes("{\"protected\":99,\"opaque\":18446744073709551615}");
        compat::put(protectedFile, protectedBytes);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(
            D::group({{"configuration", D::group({{"count", D::leaf(T::SafeInteger, 1)},
                                                  {"hidden", D::leaf(T::SafeInteger, 2)}})},
                      {"content", D::group({{"protected", D::leaf(T::SafeInteger, 9)}})}}));
        ConfigSource legacy{"legacy", {"configuration"}, {{"count", 3}, {"hidden", 8}}};
        legacy.fallbackFor = "configuration";
        options.layers = {{"legacy",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {legacy},
                           false},
                          {"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"configuration", {"configuration"}, {}}, {"content", {"content"}, {}}},
                           true}};
        options.jsonFiles = {{"user", "configuration", file}, {"user", "content", protectedFile}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *draft = store.draft({{"configuration"}});
        auto *projection = draft->readProjection();
        ConfigObservation stagedObservation;
        connect(projection, &ConfigReadProjection::operationCompleted, this,
                [&](const ConfigResult &result, const ConfigObservation &observation) {
                    if (result.operation == "stageReset")
                        stagedObservation = observation;
                });
        QVERIFY(draft->setValue({"configuration", "count"}, 12).accepted); // Pending wizard input.
        const auto inputs = store.inputs().size();
        const auto intent = store.info({"configuration", "count"}).intentRevision;
        QCOMPARE(
            compat::request(store, [&] { return draft->stageReset({{"configuration"}}); }).status,
            QString("staged"));
        QVERIFY(stagedObservation.valid());
        bool rejectedStage = false;
        store.readProjection()->synchronize(
            stagedObservation, this, [&](const ConfigReadResult &result) {
                rejectedStage = true;
                QCOMPARE(result.status(), QString("failed"));
                QCOMPARE(result.error().code, QString("read-observation"));
            });
        QTRY_VERIFY(rejectedStage); // Staged coverage never certifies committed live state.
        const auto count = projection->selectedState({"configuration", "count"});
        QCOMPARE(count.pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(count.candidateAvailable());
        QCOMPARE(count.value().toInt(), inheritedCount);
        const auto originalRaw = decodeJsonValue(original).value.toMap();
        QCOMPARE(count.targetPresent(), originalRaw.contains("count"));
        QCOMPARE(projection->selectedState({"configuration", "hidden"}).value().toInt(),
                 inheritedHidden);
        QVERIFY(!projection->selectedState({"content", "protected"}).valid());
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info({"configuration", "count"}).intentRevision, intent);
        QCOMPARE(compat::bytes(file), original);
        QCOMPARE(compat::bytes(protectedFile), protectedBytes);
        QVERIFY(draft->discard());
        QCOMPARE(compat::bytes(file), original);
        QVERIFY(!draft->dirty());
        QCOMPARE(projection->selectedState({"configuration", "count"}).pending(),
                 ConfigSelectedState::Pending::None);
        QCOMPARE(
            compat::request(store, [&] { return draft->stageReset({{"configuration"}}); }).status,
            QString("staged"));
        const auto applied = compat::request(store, [&] { return draft->apply(); });
        QVERIFY(applied.status == "saved" || applied.status == "nothing_to_save");
        const auto decoded = decodeJsonValue(compat::bytes(file));
        QVERIFY(decoded.ok());
        QVERIFY(!decoded.value.toMap().contains("count"));
        QVERIFY(!decoded.value.toMap().contains("hidden"));
        QCOMPARE(decoded.value.toMap().value("opaque"), originalRaw.value("opaque"));
        QCOMPARE(compat::bytes(protectedFile), protectedBytes);
        ConfigStore cold(options);
        QTRY_COMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(cold.value({"configuration", "count"}).toInt(), inheritedCount);
        QCOMPARE(cold.value({"configuration", "hidden"}).toInt(), inheritedHidden);
        QCOMPARE(cold.value({"content", "protected"}).toInt(), 99);
    }
    void orderedStoreRead_data() {
        QTest::addColumn<int>("value");
        QTest::newRow("changed") << 3;
        QTest::newRow("equal") << 4;
    }
    void orderedStoreRead() {
        QFETCH(int, value);
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"count", D::leaf(T::SafeInteger, 4)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {}}},
                           true}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *read = store.readProjection();
        ConfigObservation write;
        connect(read, &ConfigReadProjection::operationCompleted, this,
                [&](const ConfigResult &result, const ConfigObservation &observation) {
                    if (result.operation == "save")
                        write = observation;
                });
        QCOMPARE(compat::request(store, [&] { return store.setUser({"count"}, value).requestId; })
                     .status,
                 QString("accepted"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(write.valid());
        bool signalled = false, called = false;
        connect(read, &ConfigReadProjection::completed, this, [&](const ConfigReadResult &result) {
            signalled = true;
            QCOMPARE(result.status(), QString("observed"));
            QCOMPARE(result.snapshot().selectedState({"count"}).value().toInt(), value);
        });
        read->synchronize(write, this, [&](const ConfigReadResult &result) {
            QVERIFY(signalled);
            QCOMPARE(result.status(), QString("observed"));
            QCOMPARE(store.value({"count"}).toInt(), value);
            called = true;
        });
        QVERIFY(!called);
        QTRY_VERIFY(called);
        const auto snapshot = read->snapshot();
        QCOMPARE(
            compat::request(store, [&] { return store.setUser({"count"}, 7).requestId; }).status,
            QString("accepted"));
        QCOMPARE(snapshot.selectedState({"count"}).value().toInt(), value);
    }
    void heldRuntimeRead_data() {
        QTest::addColumn<int>("value");
        QTest::addColumn<bool>("ownerFirst");
        QTest::addColumn<bool>("edit");
        QTest::addColumn<bool>("token");
        QTest::addColumn<bool>("newerInput");
        QTest::addColumn<bool>("acquisitionFailure");
        for (bool ownerFirst : {false, true}) {
            const auto suffix = ownerFirst ? "-owner-first" : "-base-first";
            QTest::newRow(qPrintable(QString("changed") + suffix))
                << 3 << ownerFirst << true << true << false << false;
            QTest::newRow(qPrintable(QString("equal") + suffix))
                << 4 << ownerFirst << true << true << false << false;
            QTest::newRow(qPrintable(QString("no-edit") + suffix))
                << 4 << ownerFirst << false << true << false << false;
            QTest::newRow(qPrintable(QString("no-token") + suffix))
                << 3 << ownerFirst << true << false << false << false;
            QTest::newRow(qPrintable(QString("newer-input") + suffix))
                << 3 << ownerFirst << true << true << true << false;
            QTest::newRow(qPrintable(QString("read-failure") + suffix))
                << 3 << ownerFirst << true << true << false << true;
        }
    }
    void composedSelectedCoverage_data() {
        QTest::addColumn<bool>("ownerFirst");
        QTest::addColumn<bool>("candidate");
        QTest::newRow("owner-first-view") << true << false;
        QTest::newRow("base-first-view") << false << false;
        QTest::newRow("owner-first-candidate") << true << true;
        QTest::newRow("base-first-candidate") << false << true;
    }
    void composedSelectedCoverage() {
        QFETCH(bool, ownerFirst);
        QFETCH(bool, candidate);
        ConfigStoreOptions baseOptions;
        baseOptions.autoSave = false;
        baseOptions.schema =
            ConfigSchema(D::group({{"settings", D::group({{"a", D::leaf(T::SafeInteger, 1)},
                                                          {"b", D::leaf(T::SafeInteger, 2)}})}}));
        auto row = D(T::Record);
        row.fields = {{"id", D::leaf(T::String, QString())}};
        ConfigStoreOptions ownerOptions;
        ownerOptions.autoSave = false;
        ownerOptions.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        ownerOptions.layers = {
            {"user",
             ConfigLayerKind::File,
             ConfigAdmissionPolicy::FieldFallback,
             {{"user",
               {},
               {{"rows",
                 QVariantList{QVariantMap{
                     {"id", "x"}, {"instanceSettings", QVariantMap{{"a", 4}, {"b", 5}}}}}}}}},
             true}};
        std::unique_ptr<ConfigStore> base, owner;
        if (ownerFirst) {
            owner = std::make_unique<ConfigStore>(ownerOptions);
            base = std::make_unique<ConfigStore>(baseOptions);
        } else {
            base = std::make_unique<ConfigStore>(baseOptions);
            owner = std::make_unique<ConfigStore>(ownerOptions);
        }
        QTRY_COMPARE(base->status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner->status(), ConfigStore::Status::Ready);
        ConfigOverlayOptions options;
        options.base = base->scope({"settings"}, false);
        options.ownerPath = {"rows"};
        options.ownerGeneration = owner->info({"rows"}).publicationGeneration;
        options.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
        options.identityPath = {{ConfigValueSelector::Kind::Key, "id", 0}};
        options.identityValue = "x";
        options.overrideKey = "instanceSettings";
        options.mode = candidate ? ConfigOverlayMode::Editor : ConfigOverlayMode::View;
        const auto opened = compat::request(*owner, [&] { return owner->openOverlay(options); });
        QCOMPARE(opened.status, QString("opened"));
        auto *overlay = ConfigStoreTestAccess::overlay(*owner, opened.editorId);
        auto *reader = overlay->readProjection();
        if (candidate) {
            QVERIFY(overlay->setValue({"a"}, 6).accepted);
            QTRY_VERIFY(reader->selectedState({"a"}).candidateAvailable());
            QCOMPARE(reader->selectedState({"a"}).candidateValue().toInt(), 6);
        }
        const int expected = candidate ? 6 : 4;
        const auto whole = reader->snapshot();
        const auto a = ConfigObservationAccess::select(whole, {{"a"}});
        const auto b = ConfigObservationAccess::select(whole, {{"b"}});
        QCOMPARE(a.paths(), QList<ConfigPath>{{"a"}});
        QCOMPARE(a.selectedState({"a"}).value().toInt(), expected);
        QVERIFY(!a.selectedState({"b"}).valid());
        const auto wholeDependencies = ConfigObservationAccess::dependencies(whole.observation());
        QCOMPARE(wholeDependencies.size(), 2);
        qsizetype targeted = 0;
        for (const auto &dependency : wholeDependencies)
            if (!dependency.targetIdentity.isEmpty()) {
                ++targeted;
                QVERIFY(dependency.logicalPaths.has_value());
                QCOMPARE(*dependency.logicalPaths, (QList<ConfigPath>{{"a"}, {"b"}}));
            }
        QCOMPARE(targeted, qsizetype(1));
        // Atomic owner/base cuts must not be replaced by logical setting paths.
        for (const auto &selected : {a, b}) {
            const auto dependencies = ConfigObservationAccess::dependencies(selected.observation());
            QCOMPARE(dependencies.size(), wholeDependencies.size());
            for (qsizetype i = 0; i < dependencies.size(); ++i) {
                QCOMPARE(dependencies[i].incarnation, wholeDependencies[i].incarnation);
                QCOMPARE(dependencies[i].delivery, wholeDependencies[i].delivery);
                QCOMPARE(dependencies[i].paths, wholeDependencies[i].paths);
                QCOMPARE(dependencies[i].targetIdentity, wholeDependencies[i].targetIdentity);
                QCOMPARE(dependencies[i].previewId, wholeDependencies[i].previewId);
                QCOMPARE(dependencies[i].previewLifetime, wholeDependencies[i].previewLifetime);
                QCOMPARE(dependencies[i].previewEpoch, wholeDependencies[i].previewEpoch);
            }
        }
        QVERIFY(!ConfigObservationAccess::applicable(b.observation(), a.observation()));
        QVERIFY(!ConfigObservationAccess::applicable(a.observation(), b.observation()));
        QVERIFY(!ConfigObservationAccess::covers(a.observation(), whole.observation()));
        QVERIFY(ConfigObservationAccess::covers(whole.observation(), a.observation()));
        QVERIFY(ConfigObservationAccess::covers(a.observation(), a.observation()));
        const auto restricted = ConfigObservationAccess::select(a, {{"a"}, {"b"}});
        QCOMPARE(restricted.paths(), a.paths());
        QVERIFY(!ConfigObservationAccess::covers(restricted.observation(), whole.observation()));
        bool called = false;
        ConfigReadResult observed;
        reader->synchronize(a.observation(), this, [&](const auto &result) {
            observed = result;
            called = true;
        });
        QTRY_VERIFY(called);
        QCOMPARE(observed.status(), candidate ? QString("failed") : QString("observed"));
        if (candidate)
            QCOMPARE(observed.error().code, QString("read-frozen"));
        else {
            QCOMPARE(observed.snapshot().selectedState({"a"}).value().toInt(), expected);
            QVERIFY(ConfigObservationAccess::covers(observed.observation(), a.observation()));
        }
        QObject presentation, scheduler;
        const auto identity = ConfigHandleSelection::capture(
            ConfigSelection::capture(overlay->schema(), {{}}, overlay->schema().leaves(),
                                     ConfigSelection::Target::RecordSlot, "selection"));
        auto *aReader =
            ConfigReadProjectionState::select(reader, identity.select({{"a"}}), &presentation);
        auto *bReader =
            ConfigReadProjectionState::select(reader, identity.select({{"b"}}), &presentation);
        QVERIFY(aReader && bReader);
        QCOMPARE(aReader->snapshot().paths(), QList<ConfigPath>{{"a"}});
        QCOMPARE(bReader->snapshot().paths(), QList<ConfigPath>{{"b"}});
        if (candidate) {
            QVERIFY(overlay->setValue({"a"}, 7).accepted);
            QCOMPARE(aReader->selectedState({"a"}).pending(), ConfigSelectedState::Pending::Set);
            QVERIFY(!aReader->selectedState({"a"}).candidateAvailable());
            QTRY_VERIFY(aReader->selectedState({"a"}).candidateAvailable());
            QCOMPARE(aReader->selectedState({"a"}).candidateValue().toInt(), 7);
            QCOMPARE(aReader->snapshot().paths(), QList<ConfigPath>{{"a"}});
            QVERIFY(!aReader->selectedState({"b"}).valid());
            QCOMPARE(bReader->snapshot().paths(), QList<ConfigPath>{{"b"}});
            QCOMPARE(bReader->selectedState({"b"}).candidateValue().toInt(), 5);
        } else {
            QList<ConfigWorkerPacket> held;
            std::function<void()> onHeld;
            ConfigStoreTestAccess::holdReader(*owner, overlay->editorId(), &scheduler, held,
                                              onHeld);
            bool aCalled = false, bCalled = false;
            ConfigReadResult aRead, bRead;
            const auto aId = aReader->synchronize({}, this, [&](const auto &read) {
                aRead = read;
                aCalled = true;
            });
            const auto bId = bReader->synchronize({}, this, [&](const auto &read) {
                bRead = read;
                bCalled = true;
            });
            QCOMPARE(aId, bId); // IDs are projection-local, not cross-view correlation.
            QTRY_COMPARE(held.size(), 2);
            const auto first = held.takeFirst();
            QCOMPARE(first.overlays.first().read.requestId, aId);
            ConfigStoreTestAccess::inject(*owner, first);
            QVERIFY(aCalled);
            QCOMPARE(aRead.status(), QString("observed"));
            QCOMPARE(aRead.snapshot().paths(), QList<ConfigPath>{{"a"}});
            QVERIFY(!bCalled); // Sibling capture is still held independently.
            QCOMPARE(aReader->snapshot().paths(), QList<ConfigPath>{{"a"}});
            QCOMPARE(bReader->snapshot().paths(), QList<ConfigPath>{{"b"}});
            QCOMPARE(reader->snapshot().paths(), overlay->schema().leaves());
            ConfigStoreTestAccess::releaseReader(*owner, held);
            QTRY_VERIFY(bCalled);
            QCOMPARE(bRead.status(), QString("observed"));
            QCOMPARE(bRead.snapshot().paths(), QList<ConfigPath>{{"b"}});
            const auto retained = aReader->snapshot();
            bool refused = false;
            aReader->synchronize(bReader->observation(), this, [&](const auto &read) {
                QCOMPARE(read.status(), QString("failed"));
                QCOMPARE(read.error().code, QString("read-observation"));
                refused = true;
            });
            QTRY_VERIFY(refused);
            QCOMPARE(retained.selectedState({"a"}).value().toInt(), 4);
            QVERIFY(!retained.selectedState({"b"}).valid());
        }
        QCOMPARE(compat::request(*owner, [&] { return owner->close(); }).status,
                 QString("nothing_to_save"));
        QVERIFY(!a.observation().valid());
    }
    void heldRuntimeRead() {
        QFETCH(int, value);
        QFETCH(bool, ownerFirst);
        QFETCH(bool, edit);
        QFETCH(bool, token);
        QFETCH(bool, newerInput);
        QFETCH(bool, acquisitionFailure);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto ownerFile = directory.filePath("owner.json");
        compat::put(ownerFile, "{\"rows\":[{\"id\":\"x\",\"instanceSettings\":{\"count\":4}}]}");
        ConfigStoreOptions baseOptions;
        baseOptions.autoSave = false;
        baseOptions.schema = ConfigSchema(
            D::group({{"settings", D::group({{"count", D::leaf(T::SafeInteger, 1)}})}}));
        auto row = D(T::Record);
        row.fields = {{"id", D::leaf(T::String, QString())}};
        ConfigStoreOptions ownerOptions;
        ownerOptions.autoSave = false;
        ownerOptions.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        ownerOptions.layers = {
            {"user",
             ConfigLayerKind::File,
             ConfigAdmissionPolicy::FieldFallback,
             {{"user",
               {},
               {{"rows", QVariantList{QVariantMap{
                             {"id", "x"}, {"instanceSettings", QVariantMap{{"count", 4}}}}}}}}},
             true}};
        ownerOptions.jsonFiles = {{"user", "user", ownerFile}};
        std::unique_ptr<ConfigStore> baseOwner, slotOwner;
        if (ownerFirst) {
            slotOwner = std::make_unique<ConfigStore>(ownerOptions);
            baseOwner = std::make_unique<ConfigStore>(baseOptions);
        } else {
            baseOwner = std::make_unique<ConfigStore>(baseOptions);
            slotOwner = std::make_unique<ConfigStore>(ownerOptions);
        }
        auto &base = *baseOwner;
        auto &owner = *slotOwner;
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        const auto baseOrder =
            ConfigObservationAccess::dependencies(base.readProjection()->observation())
                .first()
                .incarnation;
        const auto ownerOrder =
            ConfigObservationAccess::dependencies(owner.readProjection()->observation())
                .first()
                .incarnation;
        QCOMPARE(ownerOrder < baseOrder, ownerFirst);
        ConfigOverlayOptions options;
        options.base = base.scope({"settings"}, false);
        options.ownerPath = {"rows"};
        options.ownerGeneration = owner.info({"rows"}).publicationGeneration;
        options.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
        options.identityPath = {{ConfigValueSelector::Kind::Key, "id", 0}};
        options.identityValue = "x";
        options.overrideKey = "instanceSettings";
        options.mode = ConfigOverlayMode::Live;
        const auto runtimeResult =
            compat::request(owner, [&] { return owner.openOverlay(options); });
        QCOMPARE(runtimeResult.status, QString("opened"));
        auto *runtime = ConfigStoreTestAccess::overlay(owner, runtimeResult.editorId);
        QVERIFY(runtime);
        options.mode = ConfigOverlayMode::Editor;
        const auto writerResult =
            compat::request(owner, [&] { return owner.openOverlay(options); });
        QCOMPARE(writerResult.status, QString("opened"));
        auto *writer = ConfigStoreTestAccess::overlay(owner, writerResult.editorId);
        QVERIFY(writer);
        auto *writerProjection = writer->readProjection();
        QSignalSpy candidates(writerProjection, &ConfigReadProjection::stateChanged);
        const auto inputCount = owner.inputs().size();
        const auto originalOwner = owner.value({"rows"});
        QVERIFY(writer->unset({"count"}));
        QVERIFY(!writer->value({"count"}).isValid()); // Released editor-unset behavior.
        QCOMPARE(writerProjection->selectedState({"count"}).pending(),
                 ConfigSelectedState::Pending::Remove);
        QVERIFY(!writerProjection->selectedState({"count"}).candidateAvailable());
        QTRY_VERIFY(!candidates.isEmpty());
        const auto reset = writerProjection->selectedState({"count"});
        QVERIFY(reset.candidateAvailable());
        QVERIFY(reset.targetPresent());
        QCOMPARE(reset.candidateValue().toInt(), 1);
        QCOMPARE(reset.value().toInt(), 1);
        QCOMPARE(owner.inputs().size(), inputCount);
        QCOMPARE(owner.value({"rows"}), originalOwner); // Includes exact raw numeric atoms.
        QCOMPARE(compat::request(owner, [&] { return writer->discard(); }).status,
                 QString("discarded"));
        QCOMPARE(writer->value({"count"}).toInt(), 4);
        QCOMPARE(writerProjection->selectedState({"count"}).pending(),
                 ConfigSelectedState::Pending::None);
        QList<ConfigWorkerPacket> held;
        std::function<void()> onHeld;
        QObject scheduler;
        ConfigStoreTestAccess::holdReader(owner, runtime->editorId(), &scheduler, held, onHeld);
        ConfigObservation write;
        connect(writer->readProjection(), &ConfigReadProjection::operationCompleted, this,
                [&](const ConfigResult &result, const ConfigObservation &token) {
                    if (result.operation == "overlayApply")
                        write = token;
                });
        if (edit)
            QVERIFY(writer->setValue({"count"}, value).accepted);
        const auto saved = compat::request(owner, [&] { return writer->apply(); });
        QVERIFY(saved.status == "saved" || saved.status == "nothing_to_save");
        QCOMPARE(saved.refreshState, QString("current"));
        QVERIFY(write.valid());
        QCOMPARE(runtime->value({"count"}).toInt(), 4);
        bool called = false;
        ConfigReadResult observed;
        int readSignals = 0, callbacks = 0;
        connect(runtime->readProjection(), &ConfigReadProjection::completed, this,
                [&](const ConfigReadResult &result) {
                    ++readSignals;
                    if (result.status() == "observed")
                        QCOMPARE(runtime->value({"count"}).toInt(), newerInput ? 7 : value);
                });
        const auto synchronize = [&] {
            runtime->readProjection()->synchronize(
                token ? write : ConfigObservation(), this, [&](const ConfigReadResult &result) {
                    QCOMPARE(readSignals, callbacks + 1);
                    ++callbacks;
                    if (result.status() == "observed")
                        QCOMPARE(runtime->value({"count"}).toInt(), newerInput ? 7 : value);
                    observed = result;
                    called = true;
                });
        };
        if (acquisitionFailure) {
            const auto lastGood = runtime->readProjection()->observation();
            const auto goodBytes = compat::bytes(ownerFile);
            compat::put(ownerFile, "{");
            synchronize();
            QTRY_VERIFY(called);
            QCOMPARE(observed.status(), QString("failed"));
            QCOMPARE(runtime->readProjection()->observation(), lastGood);
            QCOMPARE(runtime->value({"count"}).toInt(), 4);
            called = false;
            compat::put(ownerFile, goodBytes);
        }
        synchronize();
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        onHeld = [&] {
            for (const auto &packet : held)
                for (const auto &p : packet.overlays)
                    if (p.read.requestId)
                        loop.quit();
        };
        deadline.start(10000);
        loop.exec();
        onHeld = {};
        bool exact = false;
        for (const auto &packet : held)
            for (const auto &p : packet.overlays)
                exact |= p.read.requestId != 0;
        QVERIFY(exact);
        if (called) {
            QProcess premature;
            premature.start(
                QCoreApplication::applicationFilePath(),
                {"--inert-daemon",
                 QString::number(observed.snapshot().selectedState({"count"}).value().toInt())});
            QVERIFY(premature.waitForFinished(10000));
            qInfo().noquote() << "GENERIC_BARRIER_BYPASS"
                              << premature.readAllStandardOutput().trimmed();
        }
        QVERIFY(!called);
        if (newerInput)
            QVERIFY(runtime->setValue({"count"}, 7).accepted);
        delete writer; // Committed coverage survives editor destruction.
        QVERIFY(write.valid());
        const auto older = held;
        ConfigStoreTestAccess::releaseReader(owner, held);
        QVERIFY(called);
        QCOMPARE(observed.status(), QString("observed"));
        QCOMPARE(runtime->value({"count"}).toInt(), newerInput ? 7 : value);
        QCOMPARE(observed.snapshot().selectedState({"count"}).value().toInt(), value);
        QCOMPARE(observed.pending(),
                 newerInput ? QList<ConfigPath>{{"count"}} : QList<ConfigPath>{});
        QProcess consumer;
        consumer.start(
            QCoreApplication::applicationFilePath(),
            {"--inert-daemon",
             QString::number(observed.snapshot().selectedState({"count"}).value().toInt())});
        QVERIFY(consumer.waitForFinished(10000));
        QCOMPARE(consumer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(consumer.exitCode(), 0);
        QCOMPARE(consumer.readAllStandardOutput().trimmed(),
                 "DAEMON_ARGV " + QByteArray::number(value));
        QCOMPARE(compat::request(owner, [&] { return runtime->setValue({"count"}, 7).requestId; })
                     .status,
                 QString("accepted"));
        ConfigStoreTestAccess::releaseReader(owner, held);
        QCOMPARE(runtime->value({"count"}).toInt(), 7);
        for (const auto &packet : older)
            ConfigStoreTestAccess::inject(owner, packet);
        QCOMPARE(runtime->value({"count"}).toInt(), 7);
        QCOMPARE(runtime->readProjection()->snapshot().selectedState({"count"}).value().toInt(), 7);
    }
    void removalCandidateUsesPublicationProjection() {
        auto row = D(T::Record);
        row.fields = {{"id", D::leaf(T::String, QString())}};
        const QVariantList inherited{QVariantMap{{"id", "visible"}},
                                     QVariantMap{{"id", "hidden"}, {"opaque", true}}};
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema = ConfigSchema(D::group({{"rows", D::list(row, inherited)}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"user", {}, {{"rows", QVariantList{QVariantMap{{"id", "visible"}}}}}}},
                           true}};
        options.recordListPolicies = {{"rows", {"rows"}, {}, "id", {}, true, false}};
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRecordListMetadata metadata;
        metadata.ready = true;
        metadata.entries = {{"visible", true, false, false, {}}};
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("rows", metadata); })
                .status,
            QString("projected"));
        auto *draft = store.draft({{"rows"}});
        QVERIFY(draft->valid());
        const auto inputs = store.inputs().size();
        const auto intent = store.info({"rows"}).intentRevision;
        QSignalSpy effects(&store, &ConfigStore::projectionEffects);
        QCOMPARE(compat::request(store, [&] { return draft->stageReset({{"rows"}}); }).status,
                 QString("staged"));
        const auto candidate = draft->previewValue({"rows"});
        QCOMPARE(candidate.toList().size(), 1);
        QCOMPARE(candidate.toList().first().toMap().value("id").toString(), QString("visible"));
        QCOMPARE(draft->value({"rows"}), candidate);
        QCOMPARE(store.inputs().size(), inputs);
        QCOMPARE(store.info({"rows"}).intentRevision, intent);
        QVERIFY(effects.isEmpty());
        QVERIFY(draft->discard());
        QCOMPARE(compat::request(store, [&] { return draft->stageReset({{"rows"}}); }).status,
                 QString("staged"));
        QCOMPARE(compat::request(store, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(store.value({"rows"}), candidate);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--inert-daemon") {
        if (app.arguments().size() != 3)
            return 2;
        std::printf("DAEMON_ARGV %s\n", qPrintable(app.arguments()[2]));
        return 0;
    }
    GenericPrimitivesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_generic_primitives.moc"
