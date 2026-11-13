// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configjsoncodec.h"
#include "confignumber.h"
#include "storeprobe.h"
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QTemporaryDir>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
ConfigPath owner(bool same, bool right = false) {
    return same ? ConfigPath{"layout"} : ConfigPath{right ? "right" : "left", "rows"};
}
ConfigStoreOptions transferOptions(bool same, const QString &directory = {}) {
    auto optional = D::leaf(T::Boolean, false);
    optional.optional = true;
    optional.defaultValue.reset();
    D row;
    row.type = T::Record;
    row.fields = {{"id", D::leaf(T::String, QString())}, {"compact", optional}};
    auto list = D::list(row, {});
    list.idField = "id";
    const QVariantList left{QVariantMap{{"id", "hidden"}, {"unknown", "hidden-data"}},
                            QVariantMap{{"id", "clock"}, {"deprecated", "retain-me"}},
                            QVariantMap{{"id", "stay"}}};
    const QVariantList right{QVariantMap{{"id", "other"}}};
    ConfigStoreOptions o;
    o.autoSave = false;
    if (same) {
        auto layout = D::leaf(T::Record, QVariantMap{{"left", left}, {"right", right}});
        layout.fields = {{"left", list}, {"right", list}};
        o.schema = ConfigSchema(D::group({{"layout", layout}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        if (!directory.isEmpty())
            o.jsonFiles = {{"user", "user", directory + "/user.json"}};
    } else {
        auto leftList = list, rightList = list;
        leftList.defaultValue = left;
        rightList.defaultValue = right;
        o.schema = ConfigSchema(D::group({{"left", D::group({{"rows", leftList}})},
                                          {"right", D::group({{"rows", rightList}})}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"left", {"left"}, {}}, {"right", {"right"}, {}}},
                     true}};
        if (!directory.isEmpty())
            o.jsonFiles = {{"user", "left", directory + "/left.json"},
                           {"user", "right", directory + "/right.json"}};
    }
    for (const bool rightSide : {false, true})
        o.recordListPolicies.append(
            {rightSide ? "right" : "left",
             owner(same, rightSide),
             same ? QList<ConfigValueSelector>{{ConfigValueSelector::Kind::Key,
                                                rightSide ? "right" : "left", 0}}
                  : QList<ConfigValueSelector>{},
             "id",
             {},
             true,
             true});
    return o;
}
ConfigRecordListMetadata transferMetadata() {
    ConfigRecordListMetadata metadata;
    metadata.ready = true;
    for (const auto &key : {"clock", "stay", "other"})
        metadata.entries.append({key, true, false, true, {{"compact", false}}});
    return metadata;
}
ConfigRecordListEditOptions transfer(ConfigStore &store, bool same) {
    ConfigRecordListEdit edit;
    edit.op = ConfigRecordListEdit::Op::Transfer;
    const auto address = [](const char *key) {
        return QList<ConfigValueSelector>{{ConfigValueSelector::Kind::Key, key, 0}};
    };
    edit.from = {"left", same ? address("left") : QList<ConfigValueSelector>{},
                 store.info(owner(same)).publicationGeneration, 0};
    edit.destination = {"right", same ? address("right") : QList<ConfigValueSelector>{},
                        store.info(owner(same, true)).publicationGeneration, 1};
    return {edit.from.generation, edit.from.listAddress, {edit}};
}
QVariantList displayed(ConfigStore &store, bool same, bool right = false) {
    const auto value = store.value(owner(same, right));
    return same ? value.toMap().value(right ? "right" : "left").toList() : value.toList();
}
} // namespace
class RecordListTest : public QObject {
    Q_OBJECT
private slots:
    void ownSaveContinuation_data() {
        QTest::addColumn<QString>("change");
        for (const auto *change :
             {"none", "foreign", "foreign-equal", "foreign-aba", "local-equal", "metadata-equal",
              "numeric-storage", "failed-write", "uncertain-readback"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void ownSaveContinuation() {
        QFETCH(QString, change);
        QTemporaryDir dir;
        const auto file = dir.filePath("user.json");
        const QByteArray original =
            "{\"rows\":[{\"id\":\"x\",\"label\":\"old\"}],\"wide\":18446744073709551615}";
        compat::put(file, original);
        auto row = D::leaf(T::Record, QVariantMap{{"id", "x"}});
        row.fields = {{"id", D::leaf(T::String, QString())}};
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"rows", D::list(row, {})}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        o.jsonFiles = {{"user", "user", file}};
        auto armed = std::make_shared<bool>(true);
        o.fileFault = [armed, change](const QString &phase, const QString &) {
            if (*armed && ((change == "failed-write" && phase == "write") ||
                           (change == "uncertain-readback" && phase == "readback"))) {
                *armed = false;
                return QString("once");
            }
            return QString();
        };
        o.recordListPolicies = {{"rows", {"rows"}, {}, "id"}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const ConfigRecordListEdit edit{
            ConfigRecordListEdit::Op::Set, 0,
            change == "numeric-storage" ? QString("number") : QString("label"),
            change == "numeric-storage" ? QVariant(17) : QVariant(QString("edited"))};
        QCOMPARE(compat::request(store,
                                 [&] {
                                     return store.editRecordList(
                                         "rows",
                                         {store.info({"rows"}).publicationGeneration, {}, {edit}});
                                 })
                     .status,
                 QString("admitted"));
        const auto captured = store.info({"rows"});
        if (change == "failed-write" || change == "uncertain-readback") {
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                     QString("failed"));
            QCOMPARE(store.info({"rows"}).publicationGeneration, captured.publicationGeneration);
        }
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        const auto saved = store.info({"rows"});
        QCOMPARE(saved.publicationGeneration, captured.publicationGeneration);
        QCOMPARE(saved.contentRevision, captured.contentRevision);
        QCOMPARE(saved.pathGeneration, captured.pathGeneration);
        QCOMPARE(saved.intentRevision, captured.intentRevision);
        QCOMPARE(saved.provenance.contributions.last().localIntentRevision, quint64(0));
        QVERIFY(saved.provenance.contributions.last().revision !=
                captured.provenance.contributions.last().revision);
        if (change.startsWith("foreign")) {
            const auto ownBytes = compat::bytes(file);
            compat::put(file, change == "foreign-equal"
                                  ? ownBytes
                                  : QByteArray("{\"rows\":[{\"id\":\"x\",\"label\":\"foreign\"}],"
                                               "\"wide\":18446744073709551615}"));
            QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                     QString("reloaded"));
            if (change == "foreign-aba") {
                compat::put(file, ownBytes);
                QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                         QString("reloaded"));
            }
        } else if (change == "local-equal")
            compat::request(
                store, [&] { return store.setUser({"rows"}, store.value({"rows"})).requestId; });
        else if (change == "metadata-equal")
            QCOMPARE(
                compat::request(store, [&] { return store.updateRecordListPolicy("rows", {}); })
                    .status,
                QString("projected"));
        ConfigRecordListEdit remove;
        remove.op = ConfigRecordListEdit::Op::Remove;
        const auto result = compat::request(store, [&] {
            return store.editRecordList("rows", {captured.publicationGeneration, {}, {remove}});
        });
        const bool stale = change != "none" && change != "numeric-storage" &&
                           change != "failed-write" && change != "uncertain-readback";
        if (stale) {
            QCOMPARE(result.error.code, QString("list-edit-conflict"));
            QCOMPARE(store.value({"rows"}).toList().size(), 1);
        } else {
            QCOMPARE(result.status, QString("admitted"));
            QVERIFY(store.value({"rows"}).toList().isEmpty());
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
            QVERIFY(ConfigJsonCodec()
                        .decode(compat::bytes(file), "user")
                        .raw.value("rows")
                        .toList()
                        .isEmpty());
        }
        QCOMPARE(ConfigJsonCodec()
                     .decode(compat::bytes(file), "user")
                     .raw.value("wide")
                     .value<ConfigNumber>()
                     .token(),
                 QString("18446744073709551615"));
    }
    void transferAfterOwnSave_data() {
        QTest::addColumn<bool>("foreign");
        QTest::newRow("own-confirmation") << false;
        QTest::newRow("foreign-after-confirmation") << true;
    }
    void rebaseBeforeOwnConfirmationStillInvalidates() {
        QTemporaryDir dir;
        const auto file = dir.filePath("left.json");
        compat::put(file, "{\"rows\":[{\"id\":\"clock\"}]}");
        auto o = transferOptions(false, dir.path());
        o.evidenceDirectory = dir.filePath("evidence");
        o.jsonFiles[0].conflictPolicy = ConfigConflictPolicy::StrictPendingRebase;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRecordListEdit set{ConfigRecordListEdit::Op::Set, 0, "label", QString("edited")};
        QCOMPARE(compat::request(
                     store,
                     [&] {
                         return store.editRecordList(
                             "left", {store.info(owner(false)).publicationGeneration, {}, {set}});
                     })
                     .status,
                 QString("admitted"));
        const auto captured = store.info(owner(false)).publicationGeneration;
        compat::put(file, "{\"rows\":[{\"id\":\"clock\",\"foreign\":true}]}");
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QVERIFY(store.info(owner(false)).publicationGeneration != captured);
        ConfigRecordListEdit remove;
        remove.op = ConfigRecordListEdit::Op::Remove;
        QCOMPARE(compat::request(
                     store, [&] { return store.editRecordList("left", {captured, {}, {remove}}); })
                     .error.code,
                 QString("list-edit-conflict"));
    }
    void transferAfterOwnSave() {
        QFETCH(bool, foreign);
        QTemporaryDir dir;
        auto o = transferOptions(false, dir.path());
        compat::put(dir.filePath("left.json"),
                    "{\"rows\":[{\"id\":\"clock\",\"wide\":18446744073709551615}]}");
        compat::put(dir.filePath("right.json"), "{\"rows\":[{\"id\":\"other\"}]}");
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        for (const auto &path : {owner(false), owner(false, true)})
            QCOMPARE(compat::request(
                         store, [&] { return store.setUser(path, store.value(path)).requestId; })
                         .status,
                     QString("accepted"));
        auto edits = transfer(store, false);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(store.info(owner(false)).publicationGeneration, edits.edits[0].from.generation);
        QCOMPARE(store.info(owner(false, true)).publicationGeneration,
                 edits.edits[0].destination.generation);
        if (foreign) {
            compat::put(dir.filePath("right.json"), "{\"rows\":[{\"id\":\"changed\"}]}");
            QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                     QString("reloaded"));
        }
        const auto result =
            compat::request(store, [&] { return store.editRecordList("left", edits); });
        QVERIFY2(foreign || result.status == "admitted", qPrintable(result.error.code));
        if (foreign) {
            QCOMPARE(result.error.code, QString("list-edit-conflict"));
            QCOMPARE(store.value(owner(false)).toList().size(), 1);
        } else {
            QCOMPARE(result.status, QString("admitted"));
            QVERIFY(store.value(owner(false)).toList().isEmpty());
            QCOMPARE(store.value(owner(false, true)).toList().size(), 2);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
            const auto raw =
                ConfigJsonCodec().decode(compat::bytes(dir.filePath("right.json")), "right").raw;
            QCOMPARE(raw.value("rows")
                         .toList()
                         .last()
                         .toMap()
                         .value("wide")
                         .value<ConfigNumber>()
                         .token(),
                     QString("18446744073709551615"));
        }
    }
    void hiddenRowsStillInvalidateAfterOwnSave() {
        QTemporaryDir dir;
        const auto file = dir.filePath("left.json");
        compat::put(file, "{\"rows\":[{\"id\":\"hidden\",\"old\":true},{\"id\":\"clock\"}]}");
        auto o = transferOptions(false, dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(
            compat::request(
                store, [&] { return store.updateRecordListPolicy("left", transferMetadata()); })
                .status,
            QString("projected"));
        ConfigRecordListEdit set{ConfigRecordListEdit::Op::Set, 0, "label", QString("edited")};
        QCOMPARE(compat::request(
                     store,
                     [&] {
                         return store.editRecordList(
                             "left", {store.info(owner(false)).publicationGeneration, {}, {set}});
                     })
                     .status,
                 QString("admitted"));
        const auto captured = store.info(owner(false)).publicationGeneration;
        const auto displayed = store.value(owner(false));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        QCOMPARE(store.info(owner(false)).publicationGeneration, captured);
        compat::put(file, "{\"rows\":[{\"id\":\"hidden\",\"old\":false},{\"id\":\"clock\","
                          "\"label\":\"edited\"}]}");
        QCOMPARE(compat::request(store, [&] { return store.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(store.value(owner(false)), displayed);
        ConfigRecordListEdit remove;
        remove.op = ConfigRecordListEdit::Op::Remove;
        QCOMPARE(compat::request(
                     store, [&] { return store.editRecordList("left", {captured, {}, {remove}}); })
                     .error.code,
                 QString("list-edit-conflict"));
    }
    void openNumericRecordListUsesDeclaredDomain() {
        auto row = D::leaf(T::Record, QVariantMap{{"id", "x"}, {"n", 0.0}});
        row.fields = {{"id", D::leaf(T::String, QString())}, {"n", D::leaf(T::Real, 0.0)}};
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(
            D::group({{"rows", D::list(row, {QVariantMap{{"id", "x"}, {"n", 0.0}}})}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        o.recordListPolicies = {{"rows", {"rows"}, {}, "id", {}, false, true}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        ConfigRecordListMetadata metadata;
        metadata.ready = true;
        metadata.entries = {{"x", true, false, true, {{"n", 17}}}};
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("rows", metadata); })
                .status,
            QString("projected"));
        ConfigRecordListEdit edit;
        edit.field = "n";
        auto open = D::leaf(T::Record, QVariantMap{});
        open.open = true;
        const auto captured = open.validate(QVariantMap{{"n", 17}});
        QVERIFY(captured.ok());
        edit.value = captured.value.toMap().value("n");
        ConfigRecordListEditOptions edits{store.info({"rows"}).publicationGeneration, {}, {edit}};
        QCOMPARE(compat::request(store, [&] { return store.editRecordList("rows", edits); }).status,
                 QString("admitted"));
        const auto value = store.value({"rows"}).toList().first().toMap().value("n");
        QCOMPARE(value.metaType().id(), QMetaType::Double);
        QCOMPARE(value.toDouble(), 17.0);
        edits.generation = store.info({"rows"}).publicationGeneration;
        edits.edits[0].value = QString("17");
        QCOMPARE(compat::request(store, [&] { return store.editRecordList("rows", edits); }).status,
                 QString("failed"));
        QCOMPARE(store.value({"rows"}).toList().first().toMap().value("n").toDouble(), 17.0);
    }
    void samePolicyAddressesAndSameList() {
        auto o = transferOptions(true);
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        for (const auto &id : {"left", "right"})
            QCOMPARE(
                compat::request(
                    store, [&] { return store.updateRecordListPolicy(id, transferMetadata()); })
                    .status,
                QString("projected"));
        auto edit = transfer(store, true);
        edit.edits[0].destination = edit.edits[0].from;
        edit.edits[0].destination.index = 1;
        QCOMPARE(compat::request(store, [&] { return store.editRecordList("left", edit); }).status,
                 QString("admitted"));
        QCOMPARE(displayed(store, true).last().toMap().value("id").toString(), QString("clock"));
        const auto before = store.value(owner(true));
        edit = transfer(store, true);
        edit.edits[0].destination.index = 99;
        QCOMPARE(
            compat::request(store, [&] { return store.editRecordList("left", edit); }).error.code,
            QString("list-edit-operation"));
        QCOMPARE(store.value(owner(true)), before);
        // Reuse one Each policy for concrete addresses in a single owning leaf.
        auto row = D::leaf(T::Record, QVariantMap{{"id", "x"}});
        row.fields = {{"id", D::leaf(T::String, QString())}};
        const auto list = D::list(row, {});
        auto section = D::leaf(T::Record, QVariantMap{{"rows", QVariantList{}}});
        section.fields = {{"rows", list}};
        o.schema = ConfigSchema(
            D::group({{"sections",
                       D::list(section, {QVariantMap{{"rows", QVariantList{QVariantMap{
                                                                  {"id", "x"}, {"unknown", 7}}}}},
                                         QVariantMap{{"rows", QVariantList{}}}})}}));
        o.recordListPolicies = {{"sections",
                                 {"sections"},
                                 {{ConfigValueSelector::Kind::Each, {}, 0},
                                  {ConfigValueSelector::Kind::Key, "rows", 0}},
                                 "id"}};
        ConfigStore nested(o);
        QTRY_COMPARE(nested.status(), ConfigStore::Status::Ready);
        ConfigRecordListEdit move;
        move.op = ConfigRecordListEdit::Op::Transfer;
        move.from = {"sections",
                     {{ConfigValueSelector::Kind::Index, {}, 0},
                      {ConfigValueSelector::Kind::Key, "rows", 0}},
                     nested.info({"sections"}).publicationGeneration,
                     0};
        move.destination = move.from;
        move.destination.listAddress[0].index = 1;
        ConfigRecordListEditOptions options{move.from.generation, move.from.listAddress, {move}};
        QCOMPARE(compat::request(nested, [&] { return nested.editRecordList("sections", options); })
                     .status,
                 QString("admitted"));
        const auto sections = nested.value({"sections"}).toList();
        QVERIFY(sections.first().toMap().value("rows").toList().isEmpty());
        QCOMPARE(
            sections.last().toMap().value("rows").toList().first().toMap().value("unknown").toInt(),
            7);
    }
    void transferRoundTrip_data() {
        QTest::addColumn<bool>("same");
        QTest::newRow("one-leaf") << true;
        QTest::newRow("two-shards") << false;
    }
    void transferRoundTrip() {
        QFETCH(bool, same);
        QTemporaryDir dir;
        const auto o = transferOptions(same, dir.path());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        for (const auto &id : {"left", "right"})
            QCOMPARE(
                compat::request(
                    store, [&] { return store.updateRecordListPolicy(id, transferMetadata()); })
                    .status,
                QString("projected"));
        QCOMPARE(displayed(store, same).size(), 2);
        QVERIFY(!displayed(store, same).first().toMap().contains("deprecated"));
        auto *left = same ? nullptr : store.listModel(owner(false));
        auto *right = same ? nullptr : store.listModel(owner(false, true));
        std::unique_ptr<QSignalSpy> removed, inserted, reset;
        QPersistentModelIndex staying;
        if (left) {
            removed = std::make_unique<QSignalSpy>(left, &QAbstractItemModel::rowsRemoved);
            inserted = std::make_unique<QSignalSpy>(right, &QAbstractItemModel::rowsInserted);
            reset = std::make_unique<QSignalSpy>(right, &QAbstractItemModel::modelReset);
            staying = left->index(1);
        }
        bool coherent = true;
        const auto connection =
            connect(&store, &ConfigStore::valuesChanged, &store, [&](const QList<ConfigPath> &) {
                coherent &= displayed(store, same).size() == 1 &&
                            displayed(store, same, true).size() == 2 &&
                            store.info(owner(same)).publicationGeneration ==
                                store.info(owner(same, true)).publicationGeneration;
            });
        auto edit = transfer(store, same);
        const auto receipt =
            compat::request(store, [&] { return store.editRecordList("left", edit); });
        QCOMPARE(receipt.status, QString("admitted"));
        QCOMPARE(receipt.coverage.size(), same ? 1 : 2);
        QVERIFY(coherent);
        disconnect(connection);
        QCOMPARE(displayed(store, same, true).last().toMap().value("id").toString(),
                 QString("clock"));
        if (left) {
            QCOMPARE(removed->size(), 1);
            QCOMPARE(inserted->size(), 1);
            QCOMPARE(reset->size(), 0);
            QCOMPARE(removed->first().at(1).toInt(), 0);
            QCOMPARE(inserted->first().at(1).toInt(), 1);
            QVERIFY(staying.isValid());
            QCOMPARE(staying.row(), 0);
            QCOMPARE(left->indexOfRow("stay"), 0);
            QCOMPARE(right->indexOfRow("clock"), 1);
        }
        const auto saved = compat::request(store, [&] { return store.save(); });
        QCOMPARE(saved.status, QString("saved"));
        QCOMPARE(saved.publications.size(), same ? 1 : 2);
        ConfigJsonCodec codec;
        const auto rawLeft =
            codec.decode(compat::bytes(dir.filePath(same ? "user.json" : "left.json")), "left").raw;
        const auto rawRight =
            same ? rawLeft : codec.decode(compat::bytes(dir.filePath("right.json")), "right").raw;
        const auto source = same ? rawLeft.value("layout").toMap().value("left").toList()
                                 : rawLeft.value("rows").toList();
        const auto destination = same ? rawRight.value("layout").toMap().value("right").toList()
                                      : rawRight.value("rows").toList();
        QCOMPARE(source.first().toMap().value("unknown").toString(), QString("hidden-data"));
        QCOMPARE(source.size(), 2);
        QCOMPARE(destination.last().toMap().value("deprecated").toString(), QString("retain-me"));
        QVERIFY(!destination.last().toMap().contains("compact"));
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(displayed(restarted, same).size(), 2); // Includes the preserved hidden row.
        QCOMPARE(displayed(restarted, same, true).last().toMap(), destination.last().toMap());
        for (const auto &id : {"left", "right"})
            QCOMPARE(compat::request(
                         restarted,
                         [&] { return restarted.updateRecordListPolicy(id, transferMetadata()); })
                         .status,
                     QString("projected"));
        QCOMPARE(displayed(restarted, same), displayed(store, same));
        QCOMPARE(displayed(restarted, same, true), displayed(store, same, true));
    }
    void transferRefusals_data() {
        QTest::addColumn<QString>("reason");
        for (const auto &reason :
             {"stale-source", "stale-destination", "masked-source", "masked-destination",
              "read-only", "admission", "bad-index", "bad-address", "bad-policy", "duplicate-id",
              "late-batch", "unsettled-destination"})
            QTest::newRow(reason) << QString(reason);
    }
    void transferRefusals() {
        QFETCH(QString, reason);
        auto o = transferOptions(false);
        if (reason == "read-only")
            o.layers[0].sources.removeLast();
        if (reason.startsWith("masked")) {
            const bool right = reason.endsWith("destination");
            o.layers.append(
                {"runtime",
                 ConfigLayerKind::Runtime,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"runtime",
                   {},
                   {{right ? "right" : "left", QVariantMap{{"rows", QVariantList{}}}}}}},
                 false});
        }
        if (reason == "admission")
            o.admission = [](const ConfigInput &input) {
                return input.path == owner(false, true) ? QString("test-reject") : QString();
            };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        for (const auto &id : {"left", "right"})
            QCOMPARE(
                compat::request(
                    store, [&] { return store.updateRecordListPolicy(id, transferMetadata()); })
                    .status,
                QString("projected"));
        auto edit = transfer(store, false);
        QString expected = "list-edit-operation";
        if (reason.startsWith("stale")) {
            (reason.endsWith("source") ? edit.edits[0].from : edit.edits[0].destination)
                .generation = 0;
            expected = "list-edit-conflict";
        } else if (reason.startsWith("masked"))
            expected = "list-edit-masked";
        else if (reason == "read-only")
            expected = "no-write-route";
        else if (reason == "admission")
            expected = "test-reject";
        else if (reason == "bad-index")
            edit.edits[0].destination.index = 99;
        else if (reason == "bad-address")
            edit.edits[0].destination.listAddress = {{ConfigValueSelector::Kind::Each, {}, 0}};
        else if (reason == "bad-policy") {
            edit.edits[0].destination.policyId = "missing";
            expected = "list-policy-id";
        } else if (reason == "duplicate-id") {
            edit.edits[0].from.index = 0;
            auto initial = displayed(store, false, true);
            initial.append(QVariantMap{{"id", "clock"}});
            QVERIFY(store.setUser(owner(false, true), initial).accepted);
            QTRY_COMPARE(store.info(owner(false, true)).latestInput,
                         store.info(owner(false, true)).workerCovered);
            edit = transfer(store, false);
            expected = "duplicate-row-id";
        } else if (reason == "late-batch") {
            auto invalid = edit.edits.first();
            invalid.destination.index = 99;
            edit.edits.append(invalid);
        } else if (reason == "unsettled-destination") {
            ConfigStoreTestAccess::run(store, [](ConfigWorker &w) { w.holdInputs(true); });
            QVERIFY(store.setUser(owner(false, true), displayed(store, false, true)).accepted);
            expected = "list-edit-conflict";
        }
        const auto beforeLeft = store.value(owner(false));
        const auto beforeRight = store.value(owner(false, true));
        if (reason == "unsettled-destination")
            ConfigStoreTestAccess::run(store, [](ConfigWorker &w) { w.holdInputs(false); });
        const auto result =
            compat::request(store, [&] { return store.editRecordList("left", edit); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, expected);
        QCOMPARE(store.value(owner(false)), beforeLeft);
        QCOMPARE(store.value(owner(false, true)), beforeRight);
        QCOMPARE(store.info(owner(false)).latestInput, store.info(owner(false)).workerCovered);
        QCOMPARE(store.info(owner(false, true)).latestInput,
                 store.info(owner(false, true)).workerCovered);
    }
    void disjointPoliciesComposeOnOneLeaf() {
        D row;
        row.type = T::Record;
        row.fields = {{"id", D::leaf(T::String, QString())}};
        const QVariantList rows{QVariantMap{{"id", "keep"}}, QVariantMap{{"id", "hidden"}}};
        const auto list = D::list(row, {});
        auto record = D::leaf(T::Record, QVariantMap{{"left", rows}, {"right", rows}});
        record.fields = {{"left", list}, {"right", list}};
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(D::group({{"layout", record}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        o.recordListPolicies = {{"left",
                                 {"layout"},
                                 {{ConfigValueSelector::Kind::Key, "left", 0}},
                                 "id",
                                 {},
                                 true,
                                 false},
                                {"right",
                                 {"layout"},
                                 {{ConfigValueSelector::Kind::Key, "right", 0}},
                                 "id",
                                 {},
                                 true,
                                 false}};
        ConfigRecordListMetadata metadata{true, {{"keep", true, false, false, {}}}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("left", metadata); })
                .status,
            QString("projected"));
        QCOMPARE(
            compat::request(store, [&] { return store.updateRecordListPolicy("right", metadata); })
                .status,
            QString("projected"));
        QCOMPARE(store.value({"layout"}).toMap().value("left").toList().size(), 1);
        QCOMPARE(store.value({"layout"}).toMap().value("right").toList().size(), 1);
        QVERIFY(store.inputs().isEmpty());
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
    }
    void housekeepingEditsRawNotDisplayAndRestarts() {
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original =
            "{\"rows\":[{\"id\":\"hidden\",\"old\":7},{\"id\":\"clock\",\"deprecated\":3}]}";
        compat::put(path, original);
        auto optionalBool = D::leaf(T::Boolean, false);
        optionalBool.defaultValue.reset();
        optionalBool.optional = true;
        auto optionalInteger = D::leaf(T::SafeInteger, 0);
        optionalInteger.defaultValue.reset();
        optionalInteger.optional = true;
        D record;
        record.type = T::Record;
        record.fields = {{"id", D::leaf(T::String, QString())},
                         {"compact", optionalBool},
                         {"level", optionalInteger}};
        ConfigStoreOptions o;
        o.autoSave = false;
        o.schema = ConfigSchema(
            D::group({{"rows", D::list(record, {})}, {"other", D::leaf(T::SafeInteger, 0)}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"user", {}, {}}},
                     true}};
        o.jsonFiles = {{"user", "user", path}};
        o.recordListPolicies = {
            {"widgets", {"rows"}, {}, "id", {"id", "instanceSettings"}, true, true}};
        ConfigRecordListMetadata metadata{
            true, {{"clock", true, false, true, {{"compact", false}, {"level", 1}}}}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"rows"}).toList().size(), 2);
        const auto result = compat::request(
            store, [&] { return store.updateRecordListPolicy("widgets", metadata); });
        QCOMPARE(result.status, QString("projected"));
        QCOMPARE(store.inputs().size(), 0);
        const auto displayed = store.value({"rows"}).toList();
        QCOMPARE(displayed.size(), 1);
        QVERIFY(!displayed.first().toMap().contains("deprecated"));
        QVERIFY(displayed.first().toMap().contains("compact"));
        QCOMPARE(store.listModel({"rows"})->rowCount(), 1);
        QCOMPARE(compat::bytes(path), original);
        ConfigRecordListEditOptions edit;
        edit.generation = store.info({"rows"}).publicationGeneration;
        QVERIFY(store.setUser({"other"}, 2).accepted);
        QTRY_COMPARE(store.info({"other"}).workerCovered, quint64(1));
        QCOMPARE(store.info({"rows"}).publicationGeneration, edit.generation);
        edit.edits = {{ConfigRecordListEdit::Op::Set, 0, "level", 4}};
        QCOMPARE(
            compat::request(store, [&] { return store.editRecordList("widgets", edit); }).status,
            QString("admitted"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        ConfigJsonCodec codec;
        const auto raw = codec.decode(compat::bytes(path), "user").raw.value("rows").toList();
        QCOMPARE(raw.size(), 2);
        QCOMPARE(raw.first().toMap().value("id").toString(), QString("hidden"));
        QVERIFY(raw.last().toMap().contains("deprecated"));
        QVERIFY(!raw.last().toMap().contains("compact"));
        QCOMPARE(raw.last().toMap().value("level").value<ConfigNumber>().token(), QString("4"));
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(
            compat::request(restarted,
                            [&] { return restarted.updateRecordListPolicy("widgets", metadata); })
                .status,
            QString("projected"));
        QCOMPARE(restarted.value({"rows"}).toList().last().toMap().value("level").toInt(), 4);
        QCOMPARE(compat::request(store, [&] { return store.editRecordList("widgets", edit); })
                     .error.code,
                 QString("list-edit-conflict"));
    }
};
QTEST_GUILESS_MAIN(RecordListTest)
#include "test_record_list_policy.moc"
