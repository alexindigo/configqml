// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configfacade.h"
#include "storeprobe.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
using namespace configqml;
using Probe = ConfigStoreTestAccess;
static const ConfigPath widgets{"bar", "widgets"};

class NativeBar : public ConfigFacade {
    Q_OBJECT
    Q_PROPERTY(int n READ n WRITE setN NOTIFY nChanged)
    Q_PROPERTY(QVariantList widgets READ widgets WRITE setWidgets NOTIFY widgetsChanged)
    Q_PROPERTY(QList<int> ints READ ints WRITE setInts NOTIFY widgetsChanged)
    Q_PROPERTY(QVariant wide READ wide WRITE setWide NOTIFY wideChanged)
    Q_PROPERTY(QVariant huge READ huge WRITE setHuge NOTIFY wideChanged)
    Q_PROPERTY(QVariantList wideList READ wideList WRITE setWideList NOTIFY wideChanged)
public:
    NativeBar(ConfigStore *store) : ConfigFacade(store, {"bar"}) {
        connect(this, &ConfigFacade::settingChanged, this, [this](const auto &path) {
            if (path == ConfigPath{"n"})
                emit nChanged();
            if (path == ConfigPath{"widgets"})
                emit widgetsChanged();
            if (path == ConfigPath{"wide"} || path == ConfigPath{"huge"} ||
                path == ConfigPath{"wideList"})
                emit wideChanged();
        });
    }
    int n() const { return readSetting({"n"}).toInt(); }
    void setN(int value) { writeSetting({"n"}, value); }
    QVariantList widgets() const { return readSetting({"widgets"}).toList(); }
    void setWidgets(const QVariantList &value) { writeSetting({"widgets"}, value); }
    QVariant wide() const { return readSetting({"wide"}); }
    void setWide(const QVariant &value) { writeSetting({"wide"}, value); }
    QVariant huge() const { return readSetting({"huge"}); }
    void setHuge(const QVariant &value) { writeSetting({"huge"}, value); }
    QVariantList wideList() const { return readSetting({"wideList"}).toList(); }
    void setWideList(const QVariantList &value) { writeSetting({"wideList"}, value); }
    QList<int> ints() const {
        QList<int> out;
        for (const auto &v : widgets())
            out.append(v.toInt());
        return out;
    }
    void setInts(const QList<int> &value) {
        QVariantList list;
        for (int v : value)
            list.append(v);
        setWidgets(list);
    }
signals:
    void nChanged();
    void widgetsChanged();
    void wideChanged();
};
struct Fixture {
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    ConfigQmlStore *store = nullptr;
    Fixture() {
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_FIXTURE));
        root.reset(component.create());
        if (!root)
            qWarning().noquote() << component.errorString();
        if (root)
            store = qobject_cast<ConfigQmlStore *>(root->property("store").value<QObject *>());
    }
    QVariant run(const QString &code) {
        QVariant result;
        QMetaObject::invokeMethod(root.get(), "run", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QString, code));
        return result;
    }
    int count() const { return root->property("count").toInt(); }
};

class QmlTest : public QObject {
    Q_OBJECT
private slots:
    void facadeContract_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("dynamic") << false;
        QTest::newRow("native") << true;
    }
    void facadeContract() {
        QFETCH(bool, native);
        Fixture f;
        QVERIFY(f.root);
        QVERIFY(f.store);
        QTRY_VERIFY(f.store->ready());
        NativeBar facade(f.store->core());
        if (native)
            f.root->setProperty("nativeFacade", QVariant::fromValue<QObject *>(&facade));
        QCOMPARE(f.count(), 3);
        QSignalSpy completed(f.store->core(), &ConfigStore::completed);
        Probe::run(*f.store->core(), [](auto &w) { w.holdInputs(true); });
        f.run("active.n = 1; active.widgets.push(4); active.widgets.push(5)");
        QCOMPARE(f.count(), 5);
        QCOMPARE(f.root->property("observedN").toInt(), 1);
        QCOMPARE(f.run("JSON.stringify(active.widgets)").toString(), QString("[3,1,2,4,5]"));
        QCOMPARE(f.store->core()->inputs().size(), 3);
        QCOMPARE(f.store->core()->inputs()[1].value.toList().size(), 4);
        QCOMPARE(f.store->core()->inputs()[2].value.toList().size(), 5);
        QCOMPARE(f.store->core()->info(widgets).workerCovered, quint64(0));
        QCOMPARE(completed.size(), 0);
        Probe::run(*f.store->core(), [](auto &w) { w.releaseInputs(2); });
        QTRY_COMPARE(completed.size(), 2);
        QCOMPARE(f.count(), 5);
        Probe::run(*f.store->core(), [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(completed.size(), 3);
        QCOMPARE(f.count(), 5);
        QCOMPARE(f.store->core()->info(widgets).workerCovered, quint64(3));
        QCOMPARE(f.run("JSON.stringify(active.widgets)").toString(), QString("[3,1,2,4,5]"));
        f.run("active.n = 1"); // Equal scalar intent must reach the real setter.
        QCOMPARE(f.store->core()->inputs().size(), 4);
        QMetaObject::invokeMethod(f.root.get(), "saveNow");
        QTRY_COMPARE(completed.size(), 5);
        auto order = f.root->property("order").value<QJSValue>().toVariant().toList();
        QCOMPARE(order.last().toString(), QString("callback:5"));
        QCOMPARE(order[order.size() - 2].toString(), QString("signal:5"));
        QCOMPARE(qvariant_cast<ConfigResult>(completed.last()[0]).inputPrefix, quint64(4));
    }
    void rejectionSchedules_data() {
        QTest::addColumn<int>("reject");
        QTest::addColumn<bool>("native");
        for (bool n : {false, true})
            for (int r : {1, 2})
                QTest::newRow(qPrintable(QString("%1-%2").arg(n).arg(r))) << r << n;
    }
    void rejectionSchedules() {
        QFETCH(int, reject);
        QFETCH(bool, native);
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        NativeBar facade(f.store->core());
        if (native)
            f.root->setProperty("nativeFacade", QVariant::fromValue<QObject *>(&facade));
        Probe::run(*f.store->core(), [reject](auto &w) {
            w.holdInputs(true);
            w.setAdmission([reject](const auto &i) {
                return i.sequence == quint64(reject) ? QString("rejected-by-worker") : QString();
            });
        });
        QSignalSpy completed(f.store->core(), &ConfigStore::completed);
        int atFailure = 0;
        connect(f.store->core(), &ConfigStore::completed, this, [&](const auto &r) {
            if (r.status == "failed")
                atFailure = f.count();
        });
        f.run("active.widgets.push(4); active.widgets.push(5)");
        QCOMPARE(f.count(), 5);
        Probe::run(*f.store->core(), [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(f.count(), 5);
        Probe::run(*f.store->core(), [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(completed.size(), 2);
        QCOMPARE(f.count(), reject == 2 ? 4 : 5);
        QCOMPARE(atFailure, reject == 2 ? 4 : 5);
        QCOMPARE(f.store->core()->inputs().size(), 2);
        QVERIFY(!f.store->core()->failures().isEmpty());
    }
    void invalidAndCopyBoundaries() {
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        const auto invalid = QStringList{"cfg.bar.n = '4'",
                                         "cfg.bar.n = 11",
                                         "cfg.bar.mode = 'bad'",
                                         "cfg.bar.widgets.length = 8",
                                         "cfg.bar.widgets = [1, undefined]",
                                         "cfg.bar.safe = 9007199254740992",
                                         "cfg.bar.safe = 1.5",
                                         "cfg.bar.wide = 9007199254740992",
                                         "cfg.bar.wide = '9223372036854775808'",
                                         "cfg.bar.huge = '-1'",
                                         "cfg.bar.wideList = ['2', 3]",
                                         "let a = []; a.push(a); cfg.bar.widgets = a",
                                         "cfg.bar.widgets = [root]",
                                         "store.setUser(['typo'], 4)",
                                         "cfg.bar.real = Infinity",
                                         "cfg.bar.real = NaN",
                                         "cfg.bar.url = 'https://example.test/'"};
        for (const auto &code : invalid) {
            auto result =
                f.run("(function(){try {" + code + "; return false;} catch(e) {return true;}})()");
            QVERIFY2(result.toBool(), qPrintable(code));
        }
        QCOMPARE(f.store->core()->inputs().size(), 0);
        QCOMPARE(f.count(), 3);
        QCOMPARE(f.run("cfg.bar.safe").toLongLong(), 9007199254740991LL);
        f.run("detached.push(9)");
        QCOMPARE(f.count(), 3);
        f.run("cfg.bar.widgets = detached");
        QCOMPARE(f.count(), 4);
        f.run("let attached = cfg.bar.widgets; attached.push(8)");
        QCOMPARE(f.count(), 5);
        f.run("cfg.bar.record.items.push(4); cfg.bar.record.top = 2; cfg.bar.margins.top = 4");
        QCOMPARE(f.store->core()->inputs().last().path, (ConfigPath{"bar", "margins", "top"}));
        QCOMPARE(f.run("cfg.bar.record.items.length").toInt(), 4);
        QCOMPARE(f.run("cfg.bar.record.top").toInt(), 2);
        f.run("cfg.bar.rows[0].items.push(4); cfg.bar.rows[0].top = 2");
        QCOMPARE(f.store->core()->inputs().last().path, (ConfigPath{"bar", "rows"}));
        QCOMPARE(f.run("cfg.bar.rows[0].items.length").toInt(), 2);
        QCOMPARE(f.run("cfg.bar.rows[0].top").toInt(), 2);
        f.run("let input = [1, 2]; cfg.bar.widgets = input; input.push(99)");
        QCOMPARE(f.count(), 2);
        f.run("cfg.bar.widgets.reverse(); cfg.bar.widgets.sort()");
        QCOMPARE(f.run("cfg.bar.widgets.join(',')").toString(), QString("1,2"));
    }
    void exactIntegersAndPersistence_data() { facadeContract_data(); }
    void exactIntegersAndPersistence() {
        QFETCH(bool, native);
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        NativeBar facade(f.store->core());
        if (native)
            f.root->setProperty("nativeFacade", QVariant::fromValue<QObject *>(&facade));
        QCOMPARE(f.root->property("observedWide").toString(), QString("9007199254740993"));
        QVERIFY(
            f.run("typeof cfg.bar.wide === 'string' && typeof cfg.bar.huge === 'string'").toBool());
        f.run("active.wide = '9007199254740993'; active.huge = '18446744073709551615'; "
              "active.wideList.push('9223372036854775807')");
        f.run("cfg.bar.url = Qt.resolvedUrl('https://example.test/b'); cfg.bar.nullable = null; "
              "cfg.bar.real = -0.5");
        QVERIFY(
            f.run("cfg.bar.wideList.every(function(v) {return typeof v === 'string'})").toBool());
        QSignalSpy complete(f.store->core(), &ConfigStore::completed);
        f.store->save();
        QTRY_VERIFY(!complete.isEmpty() &&
                    qvariant_cast<ConfigResult>(complete.last()[0]).operation == "save");
        QCOMPARE(qvariant_cast<ConfigResult>(complete.last()[0]).status, QString("saved"));
        QVariantMap raw;
        bool read = false;
        Probe::run(
            *f.store->core(),
            [&](auto &w) {
                auto id =
                    w.backend()->submit({ConfigBackendRequest::Operation::Read, "memory", {}, {}});
                raw = decodeMemoryValue(w.backend()->take(id)->bytes);
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QCOMPARE(valueAt(raw, {"bar", "wide"})->toLongLong(), 9007199254740993LL);
        QCOMPARE(valueAt(raw, {"bar", "huge"})->toULongLong(), std::numeric_limits<quint64>::max());
        QCOMPARE(valueAt(raw, {"bar", "wideList"})->toList().last().toLongLong(),
                 std::numeric_limits<qint64>::max());
        QCOMPARE(valueAt(raw, {"bar", "url"})->toUrl(), QUrl("https://example.test/b"));
        QVERIFY(isNull(*valueAt(raw, {"bar", "nullable"})));
        QCOMPARE(valueAt(raw, {"bar", "real"})->toDouble(), -0.5);
    }
    void nativeConversions() {
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        NativeBar facade(f.store->core());
        f.root->setProperty("nativeFacade", QVariant::fromValue<QObject *>(&facade));
        f.run("active.n = '4'; active.ints = ['oops', 2]");
        QCOMPARE(f.root->property("observedN").toInt(), 4);
        QCOMPARE(facade.ints(), (QList<int>{0, 2})); // Qt conversion happened before native setter.
        QSignalSpy rejected(&facade, &ConfigFacade::inputRejected);
        QVERIFY(
            f.run("(function(){try{active.n=11;return false;}catch(e){return true;}})()").toBool());
        QVERIFY(f.run("(function(){try{active.wide=4;return false;}catch(e){return true;}})()")
                    .toBool());
        QCOMPARE(rejected.size(), 2);
    }
    void maskedQmlAndChangedProvenance() {
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        f.store->setRuntime(widgets, QVariantList{3, 1, 2});
        QTRY_COMPARE(f.store->core()->info(widgets).provenance.maskedBy, QString("runtime"));
        Probe::run(*f.store->core(), [](auto &w) { w.holdInputs(true); });
        f.run("cfg.bar.widgets.push(4); cfg.bar.widgets.push(5)");
        QCOMPARE(f.count(), 3);
        auto inputs = f.store->core()->inputs();
        QCOMPARE(inputs[1].value.toList().size(), 4);
        QCOMPARE(inputs[2].value.toList().size(), 4);
        QCOMPARE(inputs[2].value.toList().last().toLongLong(), 5);
        Probe::run(*f.store->core(), [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(f.store->core()->info(widgets).workerCovered, quint64(3));
        f.store->clearRuntime(widgets);
        QTRY_COMPARE(f.count(), 4);
    }
    void lifetimeAndIndependentEngines() {
        Fixture a, b;
        QVERIFY(a.root && b.root);
        QTRY_VERIFY(a.store->ready() && b.store->ready());
        a.run("cfg.bar.widgets.push(4)");
        QCOMPARE(a.count(), 4);
        QCOMPARE(b.count(), 3);
        a.run("cfg.bar.n = 1; store.close()");
        QVERIFY(
            a.run("(function(){try{cfg.bar.n=2;return false;}catch(e){return true;}})()").toBool());
        QTRY_COMPARE(a.store->status(), ConfigQmlStore::Closed);
        QCOMPARE(b.store->status(), ConfigQmlStore::Ready);
    }
    void reentrantQmlPrefix_data() { facadeContract_data(); }
    void reentrantQmlPrefix() {
        QFETCH(bool, native);
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        NativeBar facade(f.store->core());
        if (native)
            f.root->setProperty("nativeFacade", QVariant::fromValue<QObject *>(&facade));
        f.root->setProperty("reenter", true);
        QSignalSpy complete(f.store->core(), &ConfigStore::completed);
        f.run("active.n = 1");
        QCOMPARE(f.root->property("observedN").toInt(), 2);
        QTRY_COMPARE(complete.size(), 3);
        auto result = qvariant_cast<ConfigResult>(complete[1][0]);
        QCOMPARE(result.operation, QString("save"));
        QCOMPARE(result.inputPrefix, quint64(1));
        bool read = false;
        QVariantMap raw;
        Probe::run(
            *f.store->core(),
            [&](auto &w) {
                auto id =
                    w.backend()->submit({ConfigBackendRequest::Operation::Read, "memory", {}, {}});
                raw = decodeMemoryValue(w.backend()->take(id)->bytes);
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QCOMPARE(valueAt(raw, {"bar", "n"})->toLongLong(), 1);
        QCOMPARE(f.store->core()->inputs().size(), 2);
    }
    void delayedMaskAndRejectedMaskedWrite() {
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        Probe::run(*f.store->core(), [](auto &w) {
            w.holdReplies(true);
            w.setAdmission([](const auto &i) {
                return i.kind == ConfigInput::Kind::User ? QString("masked-rejection") : QString();
            });
        });
        QSignalSpy complete(f.store->core(), &ConfigStore::completed);
        f.store->setRuntime(widgets, QVariantList{3, 1, 2});
        f.run("cfg.bar.widgets.push(4)");
        QCOMPARE(f.count(), 4); // Last eligible provenance was unmasked.
        Probe::run(*f.store->core(), [](auto &w) { w.releaseReplies(1); });
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(f.count(), 4);
        Probe::run(*f.store->core(), [](auto &w) { w.releaseReplies(1); });
        QTRY_COMPARE(complete.size(), 2);
        QCOMPARE(f.count(), 3);
        QCOMPARE(qvariant_cast<ConfigResult>(complete[1][0]).status, QString("failed"));
        QCOMPARE(f.store->core()->info(widgets).provenance.maskedBy, QString("runtime"));
        f.run("cfg.bar.widgets.push(5)");
        QCOMPARE(f.count(), 3);
        Probe::run(*f.store->core(), [](auto &w) { w.releaseReplies(1); });
        QTRY_COMPARE(complete.size(), 3);
        QCOMPARE(f.count(), 3);
        QCOMPARE(qvariant_cast<ConfigResult>(complete[2][0]).description(),
                 QString("masked by runtime"));
    }
    void topologyAndGenerationFences() {
        Fixture f;
        QVERIFY(f.root);
        QTRY_VERIFY(f.store->ready());
        auto *core = f.store->core();
        Probe::run(*core, [](auto &w) { w.holdInputs(true); });
        f.run("cfg.bar.widgets.push(4)");
        QCOMPARE(f.count(), 4);
        ConfigWorkerPacket packet;
        auto state = core->info(widgets);
        state.workerCovered = state.latestInput;
        state.publicationGeneration = 100;
        state.provenance.maskedBy = "stale-mask";
        packet.snapshot.values[widgets] = QVariantList{9};
        packet.snapshot.information[widgets] = state;
        packet.snapshot.topology = 2;
        Probe::inject(*core, packet);
        QCOMPARE(f.count(), 4);
        packet.snapshot.topology = 1;
        packet.snapshot.information[widgets].pathGeneration = 0;
        Probe::inject(*core, packet);
        QCOMPARE(f.count(), 4);
        packet.snapshot.information[widgets] = state;
        packet.snapshot.information[widgets].workerCovered = 0;
        Probe::inject(*core, packet);
        QCOMPARE(f.count(), 4);
        QCOMPARE(core->info(widgets).provenance.maskedBy, QString());
    }
    void qmlReadyError() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        component.setData(
            "import QtQml\nimport ConfigQml 1.0\nConfigStore { schema: ConfigSchema { ConfigValue "
            "{ key: \"n\"; valueType: ConfigValue.Integer; defaultValue: 9007199254740992 } } }",
            QUrl("file:invalid.qml"));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *store = qobject_cast<ConfigQmlStore *>(object.get());
        QVERIFY(store);
        QCOMPARE(store->status(), ConfigQmlStore::Error);
        QVERIFY(!store->error().isEmpty());
    }
};
QTEST_GUILESS_MAIN(QmlTest)
#include "test_qml.moc"
