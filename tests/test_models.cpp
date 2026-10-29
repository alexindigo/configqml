// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configlistmodel.h"
#include "storeprobe.h"
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>
#include <QPersistentModelIndex>
#include <atomic>
#include <QElapsedTimer>
#include "modelbudgets.h"
#include "confignumber.h"
#include <random>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
using Probe = ConfigStoreTestAccess;
class ScalarView final : public ConfigValueInput {
public:
    explicit ScalarView(QVariant value) : m_value(std::move(value)) {}
    Kind kind() const override { return Kind::Scalar; }
    QVariant scalar() const override { return m_value; }
    qsizetype count() const override { return 0; }
    void visit(const Visitor &) const override {}

private:
    QVariant m_value;
};

static QVariantMap row(QString id, QString title = {}, int n = 1) {
    return {{"key", id}, {"title", title}, {"nested", QVariantMap{{"n", n}}}};
}
static ConfigStoreOptions options() {
    auto nested = D::leaf(T::Record, QVariantMap{{"n", 1}});
    nested.fields = {{"n", D::leaf(T::SafeInteger, 1)}};
    auto record = D::leaf(T::Record, row("a"));
    record.fields = {{"key", D::leaf(T::String, QString())},
                     {"title", D::leaf(T::String, QString())},
                     {"nested", nested}};
    auto list = D::list(record, {row("a", "A"), row("b", "B"), row("c", "C")});
    list.idField = "key";
    auto map = D::leaf(T::Map, QVariantMap{{"DP.1/a_..", row("x", "Monitor")}});
    map.element = {record};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema =
        ConfigSchema(D::group({{"rows", list},
                               {"map", map},
                               {"numbers", D::list(D::leaf(T::SafeInteger, 0), {1, 2, 3})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"memory", {}, {}}},
                 true}};
    return o;
}
static int role(ConfigListModel *model, const QByteArray &name) {
    return model->roleNames().key(name, -1);
}
static QStringList ids(ConfigListModel *model) {
    QStringList out;
    for (int i = 0; i < model->rowCount(); ++i)
        out.append(model->data(model->index(i), role(model, "rowId")).toString());
    return out;
}
class ModelsTest : public QObject {
    Q_OBJECT
private slots:
    void sealedAdmission() {
        auto o = options();
        o.schema = ConfigSchema(D::group({{"word", D::leaf(T::String, QString("initial"))},
                                          {"rows", *o.schema.descriptor({"rows"})}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QChar buffer[] = {QChar('o'), QChar('n')};
        ScalarView input(QString::fromRawData(buffer, 2));
        const auto admitted = store.schema().admit({"word"}, input);
        QVERIFY(admitted.ok());
        buffer[0] = QChar('x');
        QCOMPARE(admitted.value().toString(), QString("on"));
        auto copy = admitted.value();
        copy = "local-only";
        QVERIFY(store.setUser(admitted).accepted);
        QCOMPARE(store.value({"word"}).toString(), QString("on"));
        ConfigSchema other(D::group({{"word", D::leaf(T::String, QString())}}));
        const auto foreign = other.admit({"word"}, ScalarView(QString("foreign")));
        const auto before = store.inputs().size();
        QVERIFY(!store.setUser(foreign).accepted);
        QCOMPARE(store.inputs().size(), before);
        // Reader kind metadata cannot bypass validation of a composite payload.
        const auto forged = store.schema().admit({"rows"}, ScalarView(QVariantList{42}));
        QVERIFY(!forged.ok());
        QVERIFY(!store.setUser(forged).accepted);
        QCOMPARE(store.inputs().size(), before);
    }
    void borrowedCacheKeys() {
        auto o = options();
        auto map = D::leaf(T::Map, QVariantMap{{"key", QString("map-value")}});
        map.element = {D::leaf(T::String, QString())};
        o.schema = ConfigSchema(
            D::group({{"list", D::list(D::leaf(T::String, QString()), {QString("list-value")})},
                      {"mist", map},
                      {"gone", D::leaf(T::String, QString("stable"))}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QChar key[] = {QChar('l'), QChar('i'), QChar('s'), QChar('t')};
        QCOMPARE(store.presentation({QString::fromRawData(key, 4)}).metaType().id(),
                 int(QMetaType::QVariantList));
        key[0] = QChar('m');
        QCOMPARE(store.presentation({"mist"}).metaType().id(), int(QMetaType::QVariantMap));
        QCOMPARE(store.presentation({"mist"}), store.value({"mist"}));
        ConfigJsonLayout layout;
        QChar prefix[] = {QChar('r'), QChar('o'), QChar('o'), QChar('t')};
        QVERIFY(decodeJsonValue("{\"x\":1}", {QString::fromRawData(prefix, 4)}, &layout).ok());
        prefix[0] = QChar('b');
        QVERIFY(layout.keys.contains(ConfigPath{"root"}));
        QVERIFY(!layout.keys.contains(ConfigPath{"boot"}));
        {
            QString transient = "gone";
            store.presentation({QString::fromRawData(transient.constData(), transient.size())});
        }
        QCOMPARE(store.presentation({"gone"}).toString(), QString("stable"));
        QCOMPARE(store.presentation({"list"}).toList().first().toString(), QString("list-value"));
        Probe::run(store, [](auto &w) { w.holdInputs(true); });
        key[0] = QChar('l');
        auto write =
            store.setUser({QString::fromRawData(key, 4)}, QVariantList{QString("new-value")});
        QVERIFY(write.accepted);
        key[0] = QChar('m');
        QCOMPARE(store.value({"list"}).toList().first().toString(), QString("new-value"));
        QCOMPARE(store.info({"list"}).latestInput, write.inputSequence);
        Probe::run(store, [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(store.info({"list"}).workerCovered, write.inputSequence);
        QCOMPARE(store.presentation({"mist"}), store.value({"mist"}));
    }
    void publicNodeOwnsInput() {
        static_assert(!std::is_constructible_v<ConfigNode, D, QVariant, bool>);
        QChar text[] = {QChar('o'), QChar('n')};
        ConfigSchema schema(D::group({{"word", D::leaf(T::String, QString())}}));
        auto node = ConfigNode::fromValue(schema, {"word"}, QString::fromRawData(text, 2));
        text[0] = QChar('x');
        QCOMPARE(node.value().toString(), QString("on"));
        QChar key[] = {QChar('n')};
        QChar label[] = {QChar('o'), QChar('k')};
        auto record = D::leaf(T::Record, QVariantMap{{"n", QString()}});
        record.fields.insert(QString::fromRawData(key, 1),
                             D::leaf(T::String, QString::fromRawData(label, 2)));
        ConfigSchema nested(D::group({{"record", record}}));
        QVariantMap input{{QString::fromRawData(key, 1), QString::fromRawData(label, 2)}};
        auto snapshot = ConfigNode::fromValue(nested, {"record"}, input);
        key[0] = QChar('x');
        label[0] = QChar('x');
        input.clear();
        QCOMPARE(snapshot.keys(), QStringList{"n"});
        QCOMPARE(snapshot.get("n").value().toString(), QString("ok"));
        QChar wideKey[] = {QChar('w'), QChar('i'), QChar('d'), QChar('e')};
        auto exact = D::leaf(T::Record, QVariantMap{{"wide", qint64(7)}});
        exact.fields.insert(QString::fromRawData(wideKey, 4), D::leaf(T::Integer, qint64(7)));
        ConfigSchema exactSchema(D::group({{"record", exact}}));
        const auto exactNode = ConfigNode::fromValue(
            exactSchema, {"record"},
            QVariantMap{{QString::fromRawData(wideKey, 4), qint64(9007199254740993LL)}}, true);
        wideKey[0] = QChar('x');
        QCOMPARE(exactNode.get("wide").value().metaType().id(), int(QMetaType::QString));
        QCOMPARE(exactNode.get("wide").value().toString(), QString("9007199254740993"));
        QVERIFY(!ConfigNode::fromValue(schema, {"word"}, 42).valid());
        auto bounded = D::leaf(T::Integer, 0);
        bounded.minimum = 0;
        bounded.maximum = 10;
        bounded.choices = {1, 2};
        QVERIFY(bounded.validate(1).ok());
        QVERIFY(!bounded.validate(3).ok());
        auto real = D::leaf(T::Real, 0.0);
        real.maximum = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!real.validate(1.0).ok());
    }
    void admissionIdentity() {
        auto o = options();
        QVERIFY(o.schema.valid());
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto rejected = store.setUser({"rows"}, QVariantList{row("a"), row("a")});
        QVERIFY(!rejected.accepted);
        QCOMPARE(rejected.errors.first().code, QString("duplicate-row-id"));
        QCOMPARE(ids(store.listModel({"rows"})), (QStringList{"a", "b", "c"}));
        auto d = *o.schema.descriptor({"rows"});
        d.idField = "unknown";
        QVERIFY(!ConfigSchema(D::group({{"rows", d}})).valid());
        d.idField = "key";
        d.element[0].fields["key"].optional = true;
        QVERIFY(!ConfigSchema(D::group({{"rows", d}})).valid());
        d = *o.schema.descriptor({"rows"});
        d.element[0].nullable = true;
        QVERIFY(!ConfigSchema(D::group({{"rows", d}})).valid());
        d = *o.schema.descriptor({"rows"});
        d.defaultValue = QVariantList{row("same"), row("same")};
        QVERIFY(!ConfigSchema(D::group({{"rows", d}})).valid());
        d = *o.schema.descriptor({"rows"});
        d.element[0].fields["rowId"] = D::leaf(T::String, QString());
        QVERIFY(!ConfigSchema(D::group({{"rows", d}})).valid());
    }
    void preciseNotificationsAndSnapshots() {
        ConfigStore store(options());
        auto *model = store.listModel({"rows"});
        QCOMPARE(model, store.listModel({"rows"}));
        QVERIFY(!store.keyedModel({"rows"}));
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy moved(model, &QAbstractItemModel::rowsMoved);
        QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(model, &QAbstractItemModel::modelReset);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(model->rowCount(), 3);
        inserted.clear();
        QPersistentModelIndex selected(model->index(1));
        const auto node =
            model->data(model->index(1), role(model, "modelData")).value<ConfigNode>();
        QVERIFY(node.valid());
        const auto retained = node.get("nested").get("n");
        QCOMPARE(retained.value().toLongLong(), 1);
        bool coherent = true;
        connect(model, &QAbstractItemModel::rowsMoved, this, [&] {
            coherent &= store.value({"rows"}).toList().first().toMap().value("key") == "c";
            coherent &= QThread::currentThread() == model->thread();
        });
        store.setUser({"rows"}, QVariantList{row("c", "C"), row("b", "NEW", 7), row("a", "A")});
        QTRY_COMPARE(ids(model), (QStringList{"c", "b", "a"}));
        QCOMPARE(moved.size(), 2);
        QCOMPARE(changed.size(), 1);
        auto changedRoles = qvariant_cast<QList<int>>(changed.first().at(2));
        QVERIFY(changedRoles.contains(role(model, "title")));
        QVERIFY(changedRoles.contains(role(model, "nested")));
        QVERIFY(!changedRoles.contains(role(model, "key")));
        QCOMPARE(selected.data(role(model, "rowId")).toString(), QString("b"));
        QCOMPARE(retained.value().toLongLong(), 1);
        const auto next = model->data(model->index(1), role(model, "nested")).value<ConfigNode>();
        QCOMPARE(next.get("n").value().toLongLong(), 7);
        QVERIFY(coherent);
        QVERIFY(!model->setData(model->index(0), "hack", role(model, "title")));
        QVERIFY(!(model->flags(model->index(0)) & Qt::ItemIsEditable));
        QVERIFY(!model->insertRows(0, 1));
        QVERIFY(!model->removeRows(0, 1));
        QVERIFY(!model->moveRows({}, 0, 1, {}, 2));
        store.setUser({"rows"}, QVariantList{row("b", "NEW", 7), row("d", "D")});
        QTRY_COMPARE(ids(model), (QStringList{"b", "d"}));
        QCOMPARE(removed.size(), 2);
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(selected.row(), 0);
        QCOMPARE(reset.size(), 0);
    }
    void staleBaseAndRejection() {
        auto o = options();
        o.admission = [](const ConfigInput &i) {
            return i.sequence == 3 ? QString("denied") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *model = store.listModel({"rows"});
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QPersistentModelIndex retained(model->index(1));
        ConfigStoreTestAccess::run(store, [](auto &w) { w.holdInputs(true); });
        store.setUser({"rows"}, QVariantList{row("c")});
        store.setUser({"rows"}, QVariantList{row("b", "final"), row("a")});
        QSignalSpy complete(&store, &ConfigStore::completed);
        ConfigStoreTestAccess::run(store, [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(ids(model), (QStringList{"a", "b", "c"}));
        ConfigStoreTestAccess::run(store, [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(ids(model), (QStringList{"b", "a"}));
        QVERIFY(retained.isValid());
        QCOMPARE(retained.row(), 0);
        store.setUser({"rows"}, QVariantList{row("z")});
        bool revertedAtFailure = false;
        connect(&store, &ConfigStore::completed, this, [&](const ConfigResult &r) {
            if (r.status == "failed")
                revertedAtFailure =
                    ids(model) == QStringList{"b", "a"} &&
                    store.value({"rows"}).toList().first().toMap().value("key") == "b";
        });
        ConfigStoreTestAccess::run(store, [](auto &w) {
            w.releaseInputs(1);
            w.holdInputs(false);
        });
        QTRY_VERIFY(revertedAtFailure);
    }
    void keyedAndIndexOnly() {
        ConfigStore store(options());
        auto *map = store.keyedModel({"map"});
        auto *numbers = store.listModel({"numbers"});
        QAbstractItemModelTester a(map, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QAbstractItemModelTester b(numbers, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QPersistentModelIndex key(map->index(0));
        QPersistentModelIndex position(numbers->index(0));
        store.setUser({"map"},
                      QVariantMap{{".. /monitor.ß", row("y")}, {"DP.1/a_..", row("x", "changed")}});
        store.setUser({"numbers"}, QVariantList{3, 2});
        QTRY_COMPARE(map->rowCount(), 2);
        QTRY_COMPARE(numbers->rowCount(), 2);
        QCOMPARE(key.data(role(map, "rowId")).toString(), QString("DP.1/a_.."));
        QCOMPARE(key.row(), 1);
        QCOMPARE(position.row(), 0);
        QCOMPARE(position.data(Qt::UserRole).toLongLong(), 3);
    }
    void masksRuntimeAndReplacement() {
        auto o = options();
        o.layers.append({"runtime",
                         ConfigLayerKind::Runtime,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"runtime-memory", {}, {}}},
                         false});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *model = store.listModel({"rows"});
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy changes(model, &QAbstractItemModel::dataChanged);
        QSignalSpy moves(model, &QAbstractItemModel::rowsMoved);
        store.setRuntime({"rows"}, QVariantList{row("mask")});
        QTRY_COMPARE(ids(model), (QStringList{"mask"}));
        changes.clear();
        moves.clear();
        store.setUser({"rows"}, QVariantList{row("fallback")});
        QTRY_COMPARE(store.info({"rows"}).workerCovered, quint64(2));
        QCOMPARE(ids(model), (QStringList{"mask"}));
        QCOMPARE(changes.size(), 0);
        QCOMPARE(moves.size(), 0);
        store.clearRuntime({"rows"});
        QTRY_COMPARE(ids(model), (QStringList{"fallback"}));
        store.reset({"rows"}, "user");
        QTRY_COMPARE(ids(model), (QStringList{"a", "b", "c"}));
        QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(model, &QAbstractItemModel::rowsRemoved);
        store.setUser({"rows"}, QVariantList{row("x"), row("y"), row("z")});
        QTRY_COMPARE(ids(model), (QStringList{"x", "y", "z"}));
        QCOMPARE(inserted.size(), 3);
        QCOMPARE(removed.size(), 3);
    }
    void nestedTreeSurvivesOwner() {
        auto child = D::leaf(T::Record, QVariantMap{{"label", "leaf"}});
        child.fields = {{"label", D::leaf(T::String, QString())}};
        auto parent = D::leaf(T::Record, QVariantMap{{"children", QVariantList{}}});
        parent.fields = {{"children", D::list(child, {})}};
        auto o = options();
        o.schema = ConfigSchema(D::group(
            {{"tree", D::list(parent, {QVariantMap{{"children", QVariantList{QVariantMap{
                                                                    {"label", "leaf"}}}}}})}}));
        ConfigNode retained;
        {
            ConfigStore store(o);
            QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
            auto *model = store.listModel({"tree"});
            retained = model->data(model->index(0), Qt::UserRole).value<ConfigNode>();
            auto edited = retained.get("children").at(0).get("label").value();
            edited = "local";
            QCOMPARE(model->data(model->index(0), Qt::UserRole)
                         .value<ConfigNode>()
                         .get("children")
                         .at(0)
                         .get("label")
                         .value()
                         .toString(),
                     QString("leaf"));
            QVERIFY(!retained.get("absent").valid());
            QVERIFY(!retained.get("children").at(-1).valid());
            QVERIFY(!retained.get("children").at(1).valid());
        }
        QCOMPARE(retained.get("children").at(0).get("label").value().toString(), QString("leaf"));
    }
    void reentrantPublication() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *model = store.listModel({"rows"});
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        bool reentered = false;
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, [&] {
            if (reentered)
                return;
            reentered = true;
            store.setUser({"rows"}, QVariantList{row("latest")});
            QCoreApplication::processEvents();
        });
        store.setUser({"rows"}, QVariantList{row("first")});
        QTRY_COMPARE(ids(model), (QStringList{"latest"}));
        QVERIFY(reentered);
        QCOMPARE(store.inputs().size(), 2);
        QCOMPARE(store.value({"rows"}).toList().first().toMap().value("key").toString(),
                 QString("latest"));
    }
    void modelThreadAndDestruction() {
        auto *store = new ConfigStore(options());
        QTRY_COMPARE(store->status(), ConfigStore::Status::Ready);
        auto *model = store->listModel({"rows"});
        auto observed = std::make_shared<std::atomic<bool>>(false);
        auto *gui = QThread::currentThread();
        ConfigStoreTestAccess::run(*store, [observed, gui](auto &w) {
            QObject::connect(
                &w, &ConfigWorker::packetReady, &w,
                [observed, gui](const ConfigWorkerPacket &p) {
                    if (!p.models.isEmpty())
                        *observed = QThread::currentThread() != gui;
                },
                Qt::DirectConnection);
        });
        QPointer<ConfigStore> guard(store);
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, [store] { delete store; });
        store->setUser({"rows"}, QVariantList{});
        QTRY_VERIFY(guard.isNull());
        QVERIFY(observed->load());
    }
    void collectionBudgets_data() {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("keyed");
        QTest::newRow("list-1000") << 1000 << false;
        QTest::newRow("map-1000") << 1000 << true;
        QTest::newRow("list-10000") << 10000 << false;
        QTest::newRow("map-10000") << 10000 << true;
    }
    void resetThreshold() {
        auto o = options();
        auto descriptor = *o.schema.descriptor({"rows"});
        descriptor.defaultValue = QVariantList{};
        o.schema = ConfigSchema(D::group({{"rows", descriptor}}));
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *model = store.listModel({"rows"});
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy reset(model, &QAbstractItemModel::modelReset);
        QVariantList rows;
        for (int i = 0; i < 64; ++i)
            rows.append(row(QString::number(i)));
        store.setUser({"rows"}, rows);
        QTRY_COMPARE(model->rowCount(), 64);
        QCOMPARE(reset.size(), 0);
        rows.append(row("64"));
        store.setUser({"rows"}, rows);
        QTRY_COMPARE(model->rowCount(), 65);
        std::reverse(rows.begin(), rows.end());
        store.setUser({"rows"}, rows);
        QTRY_COMPARE(model->indexOfRow("64"), 0);
        QCOMPARE(reset.size(), 0); // 64 moves exactly: the threshold is exclusive.
        std::reverse(rows.begin(), rows.end());
        rows.append(row("65"));
        store.setUser({"rows"}, rows);
        QTRY_COMPARE(model->rowCount(), 66);
        QCOMPARE(reset.size(), 1); // 64 moves plus an insertion exceeds 64.
        QCOMPARE(model->indexOfRow("65"), 65);
        QCOMPARE(model->indexOfRow("absent"), -1);
    }
    void mixedPermutations() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *model = store.listModel({"rows"});
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QVariantList rows;
        for (int i = 0; i < 30; ++i)
            rows.append(row(QString::number(i)));
        store.setUser({"rows"}, rows);
        QTRY_COMPARE(model->rowCount(), 30);
        QPersistentModelIndex saved(model->index(7));
        std::mt19937 random(1234);
        for (int iteration = 0; iteration < 20; ++iteration) {
            std::shuffle(rows.begin(), rows.end(), random);
            QStringList expected;
            for (const auto &value : rows)
                expected.append(value.toMap().value("key").toString());
            store.setUser({"rows"}, rows);
            QTRY_COMPARE(ids(model), expected);
            QCOMPARE(saved.data(Qt::UserRole + 1).toString(), QString("7"));
            QCOMPARE(saved.row(), expected.indexOf("7"));
        }
    }
    void collectionBudgets() {
        QFETCH(int, count);
        QFETCH(bool, keyed);
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const ConfigPath path{keyed ? "map" : "rows"};
        auto *model = keyed ? store.keyedModel(path) : store.listModel(path);
        QVariantList rows;
        QVariantMap map;
        for (int i = 0; i < count; ++i) {
            const auto key = QString("k%1").arg(i, 6, 10, QChar('0'));
            rows.append(row(key, "Title", i));
            map.insert(key, rows.last());
        }
        auto submit = [&](const QVariant &value, const char *operation) {
            QElapsedTimer clock;
            clock.start();
            const auto input = store.setUser(path, value);
            const auto ingress = clock.nsecsElapsed();
            QVERIFY(input.accepted);
            QTRY_COMPARE(store.info(path).workerCovered, input.inputSequence);
            const auto apply = Probe::publicationLibraryNanos(store);
            const auto planning = Probe::planningNanos(store);
            qInfo("BUDGET %s %s rows=%d ingress_us=%lld apply_library_us=%lld worker_us=%lld "
                  "enforce=%d",
                  keyed ? "map" : "list", operation, count, ingress / 1000, apply / 1000,
                  planning / 1000, enforceModelBudgets);
            if (enforceModelBudgets) {
                if (count == 1000)
                    QVERIFY2(ingress + apply < 16000000, "library event exceeded 16ms");
                QVERIFY2(planning < (count == 1000 ? 50000000 : 500000000),
                         "worker exceeded reconciliation budget");
            }
        };
        submit(keyed ? QVariant(map) : QVariant(rows), "write");
        QCOMPARE(model->rowCount(), count);
        QSignalSpy reset(model, &QAbstractItemModel::modelReset);
        if (keyed) {
            for (auto it = map.begin(); it != map.end(); ++it)
                it.value() = row(it.key(), "Changed", 7);
            submit(map, "nested-replace");
        } else {
            std::reverse(rows.begin(), rows.end());
            submit(rows, "reverse");
            QCOMPARE(model->indexOfRow("k000000"), count - 1);
            QCOMPARE(reset.size(), 1);
            // A one-key rotation must use its minimal LIS move and preserve identity.
            QPersistentModelIndex saved(model->index(0));
            const auto first = rows.takeFirst();
            rows.append(first);
            reset.clear();
            submit(rows, "rotate");
            QCOMPARE(reset.size(), 0);
            QCOMPARE(saved.row(), count - 1);
        }
        const auto extra = row("extra", "Inserted", 9);
        if (keyed)
            map.insert("extra", extra);
        else
            rows.insert(count / 2, extra);
        submit(keyed ? QVariant(map) : QVariant(rows), "insert");
        QCOMPARE(model->rowCount(), count + 1);
        QVERIFY(model->indexOfRow("extra") >= 0);
        // Presentations are shared across every facade and never reconverted per getter.
        QElapsedTimer reads;
        reads.start();
        for (int i = 0; i < 1000; ++i)
            QVERIFY(store.presentation(path).isValid());
        const auto readNs = reads.nsecsElapsed();
        qInfo("BUDGET %s cached-presentations rows=%d 1000_getters_us=%lld", keyed ? "map" : "list",
              count, readNs / 1000);
        if (enforceModelBudgets && count == 1000)
            QVERIFY(readNs < 16000000);
        QElapsedTimer nodes;
        nodes.start();
        for (int i = 0; i < 1000; ++i) {
            const auto node =
                model->data(model->index(i), role(model, "nested")).value<ConfigNode>();
            QVERIFY(node.valid());
            QVERIFY(node.get("n").value().isValid());
        }
        const auto nodeNs = nodes.nsecsElapsed();
        qInfo("BUDGET %s nested-nodes rows=%d 1000_traversals_us=%lld", keyed ? "map" : "list",
              count, nodeNs / 1000);
        if (enforceModelBudgets && count == 1000)
            QVERIFY(nodeNs < 16000000);
    }
};
QTEST_MAIN(ModelsTest)
#include "test_models.moc"
