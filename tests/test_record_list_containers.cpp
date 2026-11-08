// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configjsoncodec.h"
#include "confignumber.h"
#include <QFile>
#include <QTemporaryDir>
using namespace configqml;
using D = ConfigDescriptor;
using S = ConfigValueSelector;
using E = ConfigRecordListEdit;
namespace {
QList<S> address(const QString &name, const QString &section = "left") {
    return {{S::Kind::Match, "name", 0, name}, {S::Kind::Key, "widgets"}, {S::Kind::Key, section}};
}
ConfigStoreOptions options(const QString &directory = {}) {
    D row(D::Type::Record);
    row.fields = {{"id", D::leaf(D::Type::String, QString())}};
    auto rows = D::list(row, {});
    rows.optional = true;
    D widgets(D::Type::Record);
    widgets.optional = true;
    widgets.fields = {{"left", rows}, {"right", rows}};
    D screen(D::Type::Record);
    screen.fields = {{"name", D::leaf(D::Type::String, QString())}, {"widgets", widgets}};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(
        D::group({{"screens", D::list(screen, {})},
                  {"global", D::list(row, {QVariantMap{{"id", "hidden"}, {"opaque", 7}},
                                           QVariantMap{{"id", "clock"}, {"deprecated", 3}}})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"user",
                   {},
                   {{"screens", QVariantList{QVariantMap{{"name", "DP-1"}, {"opaque", 9}},
                                             QVariantMap{{"name", "DP-1"}, {"opaque", 10}}}}}}},
                 true}};
    o.recordListPolicies = {{"screens", {"screens"}, {}, "name"},
                            {"left",
                             {"screens"},
                             {{S::Kind::Each}, {S::Kind::Key, "widgets"}, {S::Kind::Key, "left"}},
                             "id"},
                            {"right",
                             {"screens"},
                             {{S::Kind::Each}, {S::Kind::Key, "widgets"}, {S::Kind::Key, "right"}},
                             "id"},
                            {"global", {"global"}, {}, "id"}};
    if (!directory.isEmpty())
        o.jsonFiles = {{"user", "user", directory + "/user.json"}};
    return o;
}
ConfigRecordListEndpoint endpoint(ConfigStore &store, const QString &policy,
                                  const QList<S> &at = {}, bool ensure = false) {
    return {policy, at,
            store.info({policy == "global" ? "global" : "screens"}).publicationGeneration, 0,
            ensure};
}
ConfigResult submit(ConfigStore &store, const QString &policy, const QList<S> &at,
                    const QList<E> &edits, bool ensure = false) {
    return compat::request(store, [&] {
        return store.editRecordList(policy,
                                    {endpoint(store, policy).generation, at, edits, ensure});
    });
}
E insert() { return {E::Op::Insert, 0, {}, QVariantMap{{"id", "clock"}, {"extra", 42}}}; }
E copy(ConfigStore &store, const QString &name) {
    E edit;
    edit.op = E::Op::Copy;
    edit.from = endpoint(store, "global");
    edit.destination = endpoint(store, "left", address(name), true);
    return edit;
}
} // namespace
class ContainersTest : public QObject {
    Q_OBJECT
private slots:
    void matchAddressOwnsBorrowedStrings() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QChar storage[] = {QChar('D'), QChar('P'), QChar('-'), QChar('1')};
        auto at = address(QString::fromRawData(storage, 4));
        const auto result = compat::request(store, [&] {
            const auto id = store.editRecordList(
                "left", {endpoint(store, "left").generation, at, {insert()}, true});
            storage[3] = QChar('2');
            return id;
        });
        QCOMPARE(result.status, QString("admitted"));
        const auto screens = store.value({"screens"}).toList();
        QCOMPARE(screens.size(), 2);
        QVERIFY(screens.first().toMap().contains("widgets"));
    }
    void typedMapCreationAndNumericCopy() {
        D row(D::Type::Record);
        row.fields = {{"id", D::leaf(D::Type::String, QString())}};
        const auto rows = D::list(row, {});
        D bucket(D::Type::Record);
        bucket.fields = {{"rows", rows}};
        D map(D::Type::Map);
        map.element = {bucket};
        map.defaultValue = QVariantMap{};
        const ConfigSchema schema(D::group({{"buckets", map}, {"source", rows}}));
        const ConfigRecordListPolicySpec destination{
            "buckets", {"buckets"}, {{S::Kind::Key, "new"}, {S::Kind::Key, "rows"}}, "id"};
        const ConfigRecordListPolicySpec source{"source", {"source"}, {}, "id"};
        QVariant target = QVariantMap{};
        const auto raw = decodeJsonValue("[{\"id\":\"hidden\",\"exact\":18446744073709551615},{"
                                         "\"id\":\"clock\",\"old\":1.0000000000000001}]")
                             .value;
        E op;
        op.op = E::Op::Copy;
        op.from = {"source", {}, 0};
        op.destination = {"buckets", destination.listPath, 0, 0, true};
        ConfigRecordListMetadata metadata{true,
                                          {{"clock", true, false, true, {{"injected", false}}}}};
        auto a = source, b = destination;
        a.pruneUnknown = b.pruneUnknown = true;
        a.normalizeFields = b.normalizeFields = true;
        QCOMPARE(copyRecordListValue(a, metadata, raw, b, metadata, target, op, schema), QString());
        const auto copied = target.toMap().value("new").toMap().value("rows");
        QCOMPARE(copied, raw);
        QVERIFY(encodeJsonValue(copied).bytes.contains("18446744073709551615"));
        QVERIFY(encodeJsonValue(copied).bytes.contains("1.0000000000000001"));
        QCOMPARE(target.toMap().keys(), QStringList({"new"}));
        QCOMPARE(target.toMap().value("new").toMap().keys(), QStringList({"rows"}));
        QCOMPARE(copyRecordListValue(a, metadata, raw, b, metadata, target, op, schema),
                 QString("list-edit-operation"));
        target = QVariantMap{{"new", QVariantMap{{"rows", QVariantList{}}}}};
        QCOMPARE(copyRecordListValue(a, metadata, raw, b, metadata, target, op, schema),
                 QString("list-edit-operation"));
    }
    void requiredCreatedRecordIsAtomic() {
        auto o = options();
        auto root = *o.schema.descriptor({});
        auto screens = root.fields.value("screens");
        screens.element[0].fields["required"] = D::leaf(D::Type::String, QString());
        root.fields["screens"] = screens;
        o.schema = ConfigSchema(root);
        o.layers[0].sources[0].raw.clear();
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto before = store.value({"screens"});
        const auto result = submit(store, "left", address("new"), {insert()}, true);
        QCOMPARE(result.status, QString("failed"));
        QVERIFY(result.error.code != "list-edit-operation");
        QCOMPARE(store.value({"screens"}), before);
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
    }
    void firstMatchCreationAndRollback() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(submit(store, "left", address("DP-1"), {insert()}, true).status,
                 QString("admitted"));
        auto screens = store.value({"screens"}).toList();
        QCOMPARE(screens.size(), 2);
        QCOMPARE(screens[0].toMap().value("opaque").toInt(), 9);
        QVERIFY(screens[0].toMap().contains("widgets"));
        QVERIFY(!screens[1].toMap().contains("widgets"));
        const auto before = store.value({"screens"});
        E invalid{E::Op::Remove, 99};
        QCOMPARE(submit(store, "left", address("new"), {insert(), invalid}, true).error.code,
                 QString("list-edit-operation"));
        QCOMPARE(store.value({"screens"}), before);
        QCOMPARE(submit(store, "left", address("new"), {insert()}, true).status,
                 QString("admitted"));
        screens = store.value({"screens"}).toList();
        QCOMPARE(screens.size(), 3);
        const auto created = screens.last().toMap();
        QCOMPARE(created.keys(), QStringList({"name", "widgets"}));
        QCOMPARE(created.value("widgets").toMap().keys(), QStringList({"left"}));
    }
    void addressRefusals_data() {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 9; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void addressRefusals() {
        QFETCH(int, kind);
        auto o = options();
        auto at = address("new");
        bool ensure = true;
        QList<E> edits{insert()};
        if (kind == 0)
            ensure = false;
        if (kind == 1)
            at[0] = {S::Kind::Index, {}, 2};
        if (kind == 2)
            at[0].key = "undeclared";
        if (kind == 3)
            at[1] = {S::Kind::Match, "name", 0, "x"};
        if (kind == 4)
            edits.clear();
        if (kind >= 5) {
            at = address("DP-1");
            QVariant member;
            if (kind == 5)
                member = QVariant::fromValue(nullptr);
            if (kind == 6)
                member = 7;
            if (kind == 7)
                member = QVariantMap{{"left", QVariant::fromValue(nullptr)}};
            if (kind == 8)
                member = QVariantMap{{"left", 7}};
            o.layers[0].sources[0].raw = {
                {"screens", QVariantList{QVariantMap{{"name", "DP-1"}, {"widgets", member}}}}};
            const auto raw = o.layers[0].sources[0].raw.value("screens");
            const auto edited = editRecordListValue(o.recordListPolicies[1], {}, raw,
                                                    {0, at, edits, true}, &o.schema);
            QVERIFY(!edited.ok());
            QCOMPARE(edited.errors.first().code, QString("list-edit-operation"));
            return;
        }
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        // Invalid source values fall back, but the raw tree must never be overwritten.
        const auto before = store.value({"screens"});
        QCOMPARE(submit(store, "left", at, edits, ensure).error.code,
                 QString("list-edit-operation"));
        QCOMPARE(store.value({"screens"}), before);
    }
    void copyRawSaveRestartAndFenceOnly() {
        QTemporaryDir dir;
        compat::put(dir.filePath("user.json"), "{}");
        const auto o = options(dir.path());
        {
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            // Same source and destination display interpretation, hidden row retained raw.
            // Identity policies remain identity even when metadata is ready.
            const auto sourceGeneration = store.info({"global"}).pathGeneration;
            const auto sourcePublication = store.info({"global"}).publicationGeneration;
            QCOMPARE(submit(store, "left", address("DP-1"), {copy(store, "DP-1")}).status,
                     QString("admitted"));
            QCOMPARE(store.info({"global"}).pathGeneration, sourceGeneration);
            QCOMPARE(store.info({"global"}).publicationGeneration, sourcePublication);
            const auto before = store.value({"screens"});
            QCOMPARE(submit(store, "left", address("DP-1"), {copy(store, "DP-1")}).error.code,
                     QString("list-edit-operation"));
            QCOMPARE(store.value({"screens"}), before);
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        }
        QFile file(dir.path() + "/user.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto raw = decodeJsonValue(file.readAll()).value.toMap();
        const auto rows = raw.value("screens")
                              .toList()
                              .first()
                              .toMap()
                              .value("widgets")
                              .toMap()
                              .value("left")
                              .toList();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].toMap().value("opaque").value<ConfigNumber>().token(), QString("7"));
        QVERIFY(!rows[1].toMap().contains("injected"));
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(restarted.value({"screens"})
                     .toList()
                     .first()
                     .toMap()
                     .value("widgets")
                     .toMap()
                     .value("left")
                     .toList()
                     .size(),
                 2);
    }
    void copyDisplayGuardAndStaleness() {
        auto o = options();
        o.recordListPolicies.last().pruneUnknown = true;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(compat::request(store,
                                 [&] {
                                     return store.updateRecordListPolicy("global",
                                                                         {true, {{"clock", true}}});
                                 })
                     .status,
                 QString("projected"));
        auto op = copy(store, "new");
        const auto before = store.value({"screens"});
        QCOMPARE(submit(store, "left", address("new"), {op}).error.code,
                 QString("list-edit-conflict"));
        QCOMPARE(store.value({"screens"}), before);
        QCOMPARE(compat::request(store, [&] { return store.updateRecordListPolicy("global", {}); })
                     .status,
                 QString("projected"));
        QCOMPARE(submit(store, "left", address("new"), {op}).error.code,
                 QString("list-edit-conflict"));
        op = copy(store, "new");
        op.from.ensure = true;
        QCOMPARE(submit(store, "left", address("new"), {op}).error.code,
                 QString("list-edit-operation"));
    }
    void atAndTransferCreateAtomically() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        E first = insert();
        first.at = endpoint(store, "left", address("new"), true);
        E bad{E::Op::Remove, 50};
        const auto before = store.value({"screens"});
        QCOMPARE(submit(store, "global", {}, {first, bad}).error.code,
                 QString("list-edit-operation"));
        QCOMPARE(store.value({"screens"}), before);
        first.at = endpoint(store, "left", address("new"), true);
        E remove{E::Op::Remove, 0};
        QCOMPARE(submit(store, "global", {}, {first, remove}).status, QString("admitted"));
        E move;
        move.op = E::Op::Transfer;
        move.from = endpoint(store, "left", address("new"));
        move.destination = endpoint(store, "right", address("other", "right"), true);
        QCOMPARE(submit(store, "left", address("new"), {move}).status, QString("admitted"));
        const auto screens = store.value({"screens"}).toList();
        QVERIFY(screens[2].toMap().value("widgets").toMap().value("left").toList().isEmpty());
        QCOMPARE(screens[3]
                     .toMap()
                     .value("widgets")
                     .toMap()
                     .value("right")
                     .toList()[0]
                     .toMap()
                     .value("extra")
                     .toInt(),
                 42);
        auto stale = endpoint(store, "left", address("absent"), true);
        QCOMPARE(submit(store, "left", address("absent"), {insert()}, true).status,
                 QString("admitted"));
        QCOMPARE(compat::request(store,
                                 [&] {
                                     return store.editRecordList(
                                         "left",
                                         {stale.generation, stale.listAddress, {insert()}, true});
                                 })
                     .error.code,
                 QString("list-edit-conflict"));
    }
    void overlapDeclarations() {
        for (int kind = 0; kind < 3; ++kind) {
            auto o = options();
            if (kind == 0)
                o.recordListPolicies[0].pruneUnknown = true;
            if (kind == 1)
                o.recordListPolicies[0].normalizeFields = true;
            if (kind == 2) {
                auto duplicate = o.recordListPolicies[1];
                duplicate.policyId = "duplicate";
                o.recordListPolicies.append(duplicate);
            }
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
            QCOMPARE(store.error(), QString("list-policy-declaration"));
        }
    }
    void unusedEnvelope_data() {
        QTest::addColumn<int>("shape");
        QTest::addColumn<bool>("stale");
        for (int shape = 0; shape < 3; ++shape)
            for (bool stale : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(shape).arg(stale))) << shape << stale;
    }
    void unusedEnvelope() {
        QFETCH(int, shape);
        QFETCH(bool, stale);
        auto o = options();
        auto root = *o.schema.descriptor({});
        auto unused = root.fields.value("global");
        unused.optional = true;
        unused.defaultValue.reset();
        root.fields["unused"] = unused;
        auto target = root.fields.value("global");
        target.defaultValue = QVariantList{};
        root.fields["target"] = target;
        o.schema = ConfigSchema(root);
        if (shape != 0)
            o.layers[0].sources[0].raw["unused"] =
                shape == 1 ? QVariant(7) : QVariant::fromValue(nullptr);
        o.recordListPolicies.append({"unused", {"unused"}, {}, "id"});
        o.recordListPolicies.append({"target", {"target"}, {}, "id"});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        E move;
        move.op = E::Op::Transfer;
        move.from = {"global", {}, store.info({"global"}).publicationGeneration, 0};
        move.destination = {"target", {}, store.info({"target"}).publicationGeneration, 0};
        if (stale)
            ++move.destination.generation;
        const auto result = compat::request(store, [&] {
            return store.editRecordList("unused",
                                        {store.info({"unused"}).publicationGeneration, {}, {move}});
        });
        if (stale) {
            QCOMPARE(result.error.code, QString("list-edit-conflict"));
            QVERIFY(store.value({"target"}).toList().isEmpty());
        } else {
            QCOMPARE(result.status, QString("admitted"));
            QCOMPARE(store.value({"target"}).toList().size(), 1);
        }
        QVERIFY(!store.value({"unused"}).isValid());
    }
    void identityAncestorEdits_data() {
        QTest::addColumn<bool>("ancestorFirst");
        QTest::newRow("ancestor-first") << true;
        QTest::newRow("ancestor-last") << false;
    }
    void identityAncestorEdits() {
        QFETCH(bool, ancestorFirst);
        QTemporaryDir dir;
        const auto path = dir.filePath("user.json");
        const QByteArray original =
            "{\"screens\":[{\"name\":\"DP-1\",\"opaque\":true,\"widgets\":{\"left\":["
            "{\"id\":\"hidden\",\"opaque\":true},{\"id\":\"clock\",\"deprecated\":true}]}},"
            "{\"name\":\"prune\",\"position\":\"top\"}]}";
        compat::put(path, original);
        auto o = options(dir.path());
        auto root = *o.schema.descriptor({});
        auto position = D::leaf(D::Type::String, QString());
        position.optional = true;
        root.fields["screens"].element[0].fields["position"] = position;
        o.schema = ConfigSchema(root);
        const QString ancestor = ancestorFirst ? "a-screens" : "z-screens";
        o.recordListPolicies[0].policyId = ancestor;
        o.recordListPolicies[1].policyId = "m-left";
        o.recordListPolicies[1].pruneUnknown = true;
        o.recordListPolicies[1].normalizeFields = true;
        const ConfigRecordListMetadata metadata{
            true, {{"clock", true, false, true, {{"compact", false}}}}};
        {
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            QCOMPARE(compat::request(
                         store, [&] { return store.updateRecordListPolicy("m-left", metadata); })
                         .status,
                     QString("projected"));
            const auto seed = decodeJsonValue(original).value.toMap().value("screens").toList();
            const auto display = store.value({"screens"}).toList();
            QCOMPARE(display.size(), seed.size());
            for (qsizetype i = 0; i < seed.size(); ++i) {
                QCOMPARE(display[i].toMap().value("name"), seed[i].toMap().value("name"));
                QCOMPARE(display[i].toMap().keys(), seed[i].toMap().keys());
            }
            const auto nested = display[0].toMap().value("widgets").toMap().value("left").toList();
            QCOMPARE(nested.size(), 1);
            QCOMPARE(nested[0].toMap().value("id").toString(), QString("clock"));
            QVERIFY(nested[0].toMap().contains("compact"));
            QVERIFY(!nested[0].toMap().contains("deprecated"));
            QCOMPARE(nested[0].toMap().value("compact").toBool(), false);
            QCOMPARE(compat::bytes(path), original);
            const auto command = [&](const QList<E> &edits) {
                return compat::request(store, [&] {
                    return store.editRecordList(
                        ancestor, {store.info({"screens"}).publicationGeneration, {}, edits});
                });
            };
            QCOMPARE(command({{E::Op::Set, 0, "position", QString("left")}}).status,
                     QString("admitted"));
            QCOMPARE(command({{E::Op::Unset, 1, "position"}}).status, QString("admitted"));
            QCOMPARE(store.value({"screens"}).toList()[1].toMap().keys(), QStringList({"name"}));
            QCOMPARE(command({{E::Op::Remove, 1}}).status, QString("admitted"));
            const auto end = store.value({"screens"}).toList().size();
            QCOMPARE(
                command(
                    {{E::Op::Insert, end, {}, QVariantMap{{"name", "new"}, {"position", "right"}}}})
                    .status,
                QString("admitted"));
            QCOMPARE(compat::request(store, [&] { return store.save(); }).status, QString("saved"));
        }
        const auto saved = compat::bytes(path);
        const auto raw = decodeJsonValue(saved).value.toMap().value("screens").toList();
        QCOMPARE(raw.size(), 2);
        QCOMPARE(raw[0].toMap().value("position").toString(), QString("left"));
        QCOMPARE(raw[1].toMap().value("name").toString(), QString("new"));
        const auto rows = raw[0].toMap().value("widgets").toMap().value("left").toList();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].toMap().value("id").toString(), QString("hidden"));
        QCOMPARE(rows[1].toMap().value("id").toString(), QString("clock"));
        QVERIFY(rows[0].toMap().value("opaque").toBool());
        QVERIFY(rows[1].toMap().value("deprecated").toBool());
        QVERIFY(!rows[1].toMap().contains("compact"));
        ConfigStore restarted(o);
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(
            compat::request(restarted,
                            [&] { return restarted.updateRecordListPolicy("m-left", metadata); })
                .status,
            QString("projected"));
        const auto display = restarted.value({"screens"}).toList();
        for (qsizetype i = 0; i < raw.size(); ++i) {
            QCOMPARE(display[i].toMap().value("name"), raw[i].toMap().value("name"));
            QCOMPARE(display[i].toMap().keys(), raw[i].toMap().keys());
        }
        QCOMPARE(display[0].toMap().value("widgets").toMap().value("left").toList().size(), 1);
        QCOMPARE(compat::bytes(path), saved);
    }
    void endpointAndMatchRefusals() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        E transfer;
        transfer.op = E::Op::Transfer;
        transfer.from = endpoint(store, "global");
        transfer.from.ensure = true;
        transfer.destination = endpoint(store, "left", address("DP-1"), true);
        QCOMPARE(submit(store, "left", address("DP-1"), {transfer}).error.code,
                 QString("list-edit-operation"));
        for (const auto op : {E::Op::Transfer, E::Op::Copy}) {
            auto invalid = copy(store, "DP-1");
            invalid.op = op;
            invalid.at = endpoint(store, "global");
            QCOMPARE(submit(store, "left", address("DP-1"), {invalid}).error.code,
                     QString("list-edit-operation"));
        }
        for (bool nullable : {false, true}) {
            auto o = options();
            auto root = *o.schema.descriptor({});
            auto &name = root.fields["screens"].element[0].fields["name"];
            name.optional = !nullable;
            name.nullable = nullable;
            o.schema = ConfigSchema(root);
            const auto raw = o.layers[0].sources[0].raw.value("screens");
            const auto result =
                editRecordListValue(o.recordListPolicies[1], {}, raw,
                                    {0, address("DP-1"), {insert()}, true}, &o.schema);
            QVERIFY(!result.ok());
            QCOMPARE(result.errors.first().code, QString("list-edit-operation"));
        }
    }
    void copyCreatesOnlyFinalListWithoutEnsure() {
        auto o = options();
        o.layers[0].sources[0].raw = {
            {"screens", QVariantList{QVariantMap{{"name", "DP-1"}, {"widgets", QVariantMap{}}}}}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto op = copy(store, "DP-1");
        op.destination.ensure = false;
        QCOMPARE(submit(store, "left", address("DP-1"), {op}).status, QString("admitted"));
        QCOMPARE(store.value({"screens"}).toList().size(), 1);
        QCOMPARE(store.value({"screens"})
                     .toList()[0]
                     .toMap()
                     .value("widgets")
                     .toMap()
                     .value("left")
                     .toList()
                     .size(),
                 2);
        QVariant absent;
        QCOMPARE(copyRecordListValue(o.recordListPolicies.last(), {}, store.value({"global"}),
                                     o.recordListPolicies.last(), {}, absent, E{E::Op::Copy},
                                     o.schema),
                 QString("list-edit-operation"));
        QVERIFY(!absent.isValid());
    }
};
QTEST_GUILESS_MAIN(ContainersTest)
#include "test_record_list_containers.moc"
