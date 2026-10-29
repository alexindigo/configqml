// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configjsoncodec.h"
#include "confignumber.h"
#include <QTemporaryDir>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
class RecordListTest : public QObject {
    Q_OBJECT
private slots:
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
