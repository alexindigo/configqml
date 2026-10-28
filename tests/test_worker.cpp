// SPDX-License-Identifier: LGPL-3.0-or-later
#include "storeprobe.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QUrl>
#include <cstdio>
#include "configenvironment.h"
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
using Probe = ConfigStoreTestAccess;

static ConfigStoreOptions options(bool masked = false, bool writable = true) {
    auto number = D::leaf(T::Integer, 0);
    number.minimum = 0;
    number.maximum = 10;
    ConfigStoreOptions o;
    o.schema = ConfigSchema(D::group(
        {{"bar", D::group({{"n", number},
                           {"widgets", D::list(D::leaf(T::Integer, 0), {3, 1, 2})},
                           {"url", D::leaf(T::Url, QUrl("https://example.test/"))},
                           {"u64", D::leaf(T::UnsignedInteger,
                                           QVariant::fromValue(
                                               std::numeric_limits<quint64>::max()))}})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"memory", {}, {{"unknown", QString("preserved")}}}},
                 writable},
                {"runtime",
                 ConfigLayerKind::Runtime,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"runtime-values",
                   {},
                   masked ? QVariantMap{{"bar", QVariantMap{{"widgets", QVariantList{3, 1, 2}}}}}
                          : QVariantMap()}},
                 false}};
    return o;
}
static const ConfigPath widgets{"bar", "widgets"};
static const ConfigPath number{"bar", "n"};

class WorkerTest : public QObject {
    Q_OBJECT
private slots:
    void readYourWritesAndStaleReplies() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        Probe::run(store, [](auto &w) { w.holdInputs(true); });
        const auto a = store.setUser(widgets, QVariantList{3, 1, 2, 4});
        const auto b = store.setUser(widgets, store.value(widgets).toList() + QVariantList{5});
        QVERIFY(a.accepted && b.accepted);
        QCOMPARE(store.value(widgets).toList(),
                 (QVariantList{qint64(3), qint64(1), qint64(2), qint64(4), qint64(5)}));
        QCOMPARE(store.info(widgets).latestInput, quint64(2));
        QCOMPARE(store.info(widgets).workerCovered, quint64(0));
        QCOMPARE(store.snapshot().values.value(widgets).toList().size(), 3);
        QCOMPARE(store.inputs().size(), 2);
        QCOMPARE(store.inputs()[0].value.toList().size(), 4);
        QCOMPARE(store.inputs()[1].value.toList().size(), 5);
        QCOMPARE(complete.size(), 0);
        Probe::run(store, [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(store.value(widgets).toList().size(), 5);
        QCOMPARE(store.info(widgets).workerCovered, quint64(0));
        Probe::run(store, [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(complete.size(), 2);
        QCOMPARE(store.info(widgets).workerCovered, quint64(2));
        QCOMPARE(store.inputs().size(), 2); // Publications never echo as input.
        Probe::run(store, [](auto &w) { w.holdInputs(false); });
    }
    void rejections_data() {
        QTest::addColumn<int>("rejected");
        QTest::newRow("first-stale") << 1;
        QTest::newRow("latest") << 2;
    }
    void rejections() {
        QFETCH(int, rejected);
        auto o = options();
        auto *objectThread = QThread::currentThread();
        o.admission = [rejected, objectThread](const ConfigInput &input) {
            if (QThread::currentThread() == objectThread)
                return QString("wrong-thread");
            return input.sequence == quint64(rejected) ? QString("injected-rejection") : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        Probe::run(store, [](auto &w) { w.holdInputs(true); });
        QSignalSpy complete(&store, &ConfigStore::completed);
        QStringList order;
        int lengthAtFailure = 0;
        connect(&store, &ConfigStore::completed, this, [&](const auto &r) {
            order.append("signal");
            if (r.status == "failed")
                lengthAtFailure = store.value(widgets).toList().size();
        });
        store.setUser(widgets, QVariantList{3, 1, 2, 4});
        store.setUser(widgets, QVariantList{3, 1, 2, 4, 5}, this,
                      [&](const auto &) { order.append("callback"); });
        QCOMPARE(store.value(widgets).toList().size(), 5);
        Probe::run(store, [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(store.value(widgets).toList().size(), 5);
        Probe::run(store, [](auto &w) { w.releaseInputs(1); });
        QTRY_COMPARE(complete.size(), 2);
        QCOMPARE(store.value(widgets).toList().size(), rejected == 2 ? 4 : 5);
        QCOMPARE(lengthAtFailure, rejected == 2 ? 4 : 5);
        QCOMPARE(store.failures().first().error.code, QString("injected-rejection"));
        QCOMPARE(order, (QStringList{"signal", "signal", "callback"}));
        auto save = store.save();
        Probe::run(store, [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(complete.size(), 3);
        const auto result = qvariant_cast<ConfigResult>(complete.last().first());
        QCOMPARE(result.requestId, save);
        QCOMPARE(result.status, QString("failed"));
    }
    void singleRejectionRollsBack() {
        auto o = options();
        o.admission = [](const auto &) { return QString("no"); };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        bool called = false;
        store.setUser(number, 6, this, [&](const auto &r) {
            QCOMPARE(store.value(number).toLongLong(), 0);
            QCOMPARE(r.status, QString("failed"));
            called = true;
        });
        QCOMPARE(store.value(number).toLongLong(), 6);
        QTRY_VERIFY(called);
    }
    void maskedIntentAndChangedMask() {
        ConfigStore store(options(true));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        Probe::run(store, [](auto &w) { w.holdInputs(true); });
        store.setUser(widgets, store.value(widgets).toList() + QVariantList{4});
        store.setUser(widgets, store.value(widgets).toList() + QVariantList{5});
        QCOMPARE(store.value(widgets).toList().size(), 3);
        QCOMPARE(store.inputs()[1].value.toList(),
                 (QVariantList{qint64(3), qint64(1), qint64(2), qint64(5)}));
        auto id = store.save();
        Probe::run(store, [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(complete.size(), 3);
        QCOMPARE(store.value(widgets).toList().size(), 3);
        QCOMPARE(store.info(widgets).provenance.maskedBy, QString("runtime"));
        auto write = qvariant_cast<ConfigResult>(complete[1][0]);
        QCOMPARE(write.description(), QString("masked by runtime"));
        auto saved = qvariant_cast<ConfigResult>(complete[2][0]);
        QCOMPARE(saved.requestId, id);
        QCOMPARE(saved.status, QString("saved"));
        QVariantMap persisted;
        bool read = false;
        Probe::run(
            store,
            [&](auto &w) {
                auto request =
                    w.backend()->submit({ConfigBackendRequest::Operation::Read, "memory", {}, {}});
                persisted = decodeMemoryValue(w.backend()->take(request)->bytes);
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QCOMPARE(valueAt(persisted, widgets)->toList().size(), 4);
        QCOMPARE(persisted["unknown"].toString(), QString("preserved"));
        store.clearRuntime(widgets);
        QTRY_COMPARE(store.info(widgets).provenance.maskedBy, QString());
        QCOMPARE(store.value(widgets).toList().size(), 4);
    }
    void synchronousChecksAndOwnedInputs() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy rejected(&store, &ConfigStore::inputRejected);
        QVERIFY(!store.setUser(number, QString("4")).accepted);
        QVERIFY(!store.setUser(number, 11).accepted);
        QVERIFY(!store.setUser(widgets, QVariantList{1, QVariant()}).accepted);
        QVERIFY(!store.setUser({"undeclared"}, 1).accepted);
        QCOMPARE(rejected.size(), 4);
        QCOMPARE(store.inputs().size(), 0);
        QCOMPARE(store.value(number).toLongLong(), 0);
        QChar text[] = {QChar('o'), QChar('n')};
        auto o = options();
        o.schema = ConfigSchema(D::group({{"s", D::leaf(T::String, QString(""))}}));
        ConfigStore strings(o);
        QTRY_COMPARE(strings.status(), ConfigStore::Status::Ready);
        strings.setUser({"s"}, QString::fromRawData(text, 2));
        text[0] = QChar('x');
        QCOMPARE(strings.value({"s"}).toString(), QString("on"));
        QCOMPARE(strings.inputs().first().value.toString(), QString("on"));
    }
    void reentryExactSavePrefix() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        bool reentered = false;
        quint64 saveId = 0;
        connect(&store, &ConfigStore::valuesChanged, this, [&](const auto &) {
            if (!reentered && store.value(number).toLongLong() == 1) {
                reentered = true;
                saveId = store.save();
                store.setUser(number, 2);
            }
        });
        store.setUser(number, 1);
        QTRY_COMPARE(complete.size(), 3);
        auto saved = qvariant_cast<ConfigResult>(complete[1][0]);
        QCOMPARE(saved.requestId, saveId);
        QCOMPARE(saved.inputPrefix, quint64(1));
        QCOMPARE(saved.status, QString("saved"));
        bool read = false;
        QVariantMap raw;
        Probe::run(
            store,
            [&](auto &w) {
                auto id =
                    w.backend()->submit({ConfigBackendRequest::Operation::Read, "memory", {}, {}});
                raw = decodeMemoryValue(w.backend()->take(id)->bytes);
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QCOMPARE(valueAt(raw, number)->toLongLong(), 1);
        QCOMPARE(store.value(number).toLongLong(), 2);
        QCOMPARE(store.inputs().size(), 2);
    }
    void delayedBackendAndFaults_data() {
        QTest::addColumn<int>("fault");
        for (int i = 0; i <= 3; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void delayedBackendAndFaults() {
        QFETCH(int, fault);
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        Probe::run(store, [fault](auto &w) {
            w.scheduleBackend({true, ConfigMemoryBackend::Fault(fault)});
        });
        store.setUser(number, 1);
        store.save();
        store.setUser(number, 2);
        store.flush();
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(store.value(number).toLongLong(), 2);
        bool barrier = false;
        Probe::run(store, [](auto &w) { w.releaseBackend(); }, this, [&] { barrier = true; });
        QTRY_VERIFY(barrier);
        QTRY_COMPARE(complete.size(), 4);
        auto saved = qvariant_cast<ConfigResult>(complete[1][0]);
        auto flushed = qvariant_cast<ConfigResult>(complete[3][0]);
        QCOMPARE(saved.inputPrefix, quint64(1));
        QCOMPARE(flushed.inputPrefix, quint64(2));
        QCOMPARE(saved.status, fault ? QString("failed") : QString("saved"));
        QCOMPARE(flushed.status, fault ? QString("failed") : QString("saved"));
        QCOMPARE(saved.publicationState, fault > 1 ? QString("uncertain") : QString("known"));
        QCOMPARE(flushed.publicationState, saved.publicationState);
        if (fault)
            QVERIFY(!store.failures().isEmpty());
    }
    void closePersistsAndRefusesWrites() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        bool read = false;
        QVariantMap raw;
        // Hold replies: the worker has completed close but remains available until
        // its completion is consumed. Read actual memory bytes before release.
        Probe::run(store, [](auto &w) { w.holdReplies(true); });
        store.setUser(number, 1);
        const auto closeId = store.close();
        QVERIFY(!store.setUser(number, 2).accepted);
        Probe::run(
            store,
            [&](auto &w) {
                auto id =
                    w.backend()->submit({ConfigBackendRequest::Operation::Read, "memory", {}, {}});
                raw = decodeMemoryValue(w.backend()->take(id)->bytes);
                w.holdReplies(false);
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QTRY_COMPARE(complete.size(), 3);
        QCOMPARE(valueAt(raw, number)->toLongLong(), 1);
        auto result = qvariant_cast<ConfigResult>(complete[1][0]);
        QCOMPARE(result.requestId, closeId);
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(store.status(), ConfigStore::Status::Closed);
    }
    void emptyFlushReadonlyAndIndependentStores() {
        ConfigStore a(options()), b(options(false, false));
        QTRY_COMPARE(a.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(b.status(), ConfigStore::Status::Ready);
        QSignalSpy ac(&a, &ConfigStore::completed), bc(&b, &ConfigStore::completed);
        a.flush();
        b.close();
        QCOMPARE(ac.size(), 0);
        QCOMPARE(bc.size(), 0);
        QTRY_COMPARE(ac.size(), 1);
        QTRY_COMPARE(bc.size(), 1);
        QCOMPARE(qvariant_cast<ConfigResult>(ac[0][0]).status, QString("nothing_to_save"));
        QCOMPARE(qvariant_cast<ConfigResult>(bc[0][0]).status, QString("nothing_to_save"));
        a.setUser(number, 3);
        QCOMPARE(b.value(number).toLongLong(), 0);
    }
    void readyErrorAndWorkerFailure() {
        auto o = options();
        o.layers.append(o.layers.first());
        ConfigStore invalid(o);
        QTRY_COMPARE(invalid.status(), ConfigStore::Status::Error);
        QVERIFY(!invalid.error().isEmpty());
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        Probe::run(store,
                   [](auto &w) { w.scheduleBackend({true, ConfigMemoryBackend::Fault::None}); });
        store.setUser(number, 1);
        store.save();
        Probe::run(store, [](auto &w) { w.fail("injected-worker-loss"); });
        QTRY_COMPARE(store.status(), ConfigStore::Status::Error);
        QTRY_COMPARE(complete.size(), 2);
        QCOMPARE(qvariant_cast<ConfigResult>(complete[1][0]).status, QString("failed"));
        QCOMPARE(qvariant_cast<ConfigResult>(complete[1][0]).publicationState,
                 QString("uncertain"));
        QCOMPARE(store.inputs().size(), 1);
        QVERIFY(!store.setUser(number, 2).accepted);
        store.flush();
        QTRY_COMPARE(complete.size(), 4);
        QCOMPARE(qvariant_cast<ConfigResult>(complete[3][0]).status, QString("failed"));
        QCOMPARE(qvariant_cast<ConfigResult>(complete[3][0]).publicationState,
                 QString("uncertain"));
    }
    void groupedResetAndReentrantCompletions() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser(number, 4);
        store.setUser(widgets, QVariantList{9});
        QTRY_COMPARE(store.info(widgets).workerCovered, quint64(2));
        bool coherent = false;
        connect(&store, &ConfigStore::valuesChanged, this, [&](const auto &paths) {
            if (paths.size() == 2)
                coherent = store.value(number).toLongLong() == 0 &&
                           store.value(widgets).toList().size() == 3;
        });
        store.reset({"bar"}, "user");
        QTRY_VERIFY(coherent);
        QStringList order;
        bool reentered = false;
        connect(&store, &ConfigStore::completed, this, [&](const auto &) {
            order.append("signal");
            if (!reentered) {
                reentered = true;
                store.flush(this, [&](const auto &) { order.append("nested-callback"); });
            }
        });
        store.save(this, [&](const auto &) { order.append("callback"); });
        QTRY_COMPARE(order.size(), 4);
        QCOMPARE(order, (QStringList{"signal", "callback", "signal", "nested-callback"}));
    }
    void timingsAndAffinity() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        Probe::run(store, [](auto &w) { w.holdInputs(true); });
        QElapsedTimer timer;
        qint64 worst = 0;
        for (int i = 0; i < 500; ++i) {
            timer.start();
            store.setUser(number, i % 11);
            worst = qMax(worst, timer.nsecsElapsed());
        }
        bool affinity = true;
        connect(&store, &ConfigStore::completed, this,
                [&](const auto &) { affinity &= QThread::currentThread() == thread(); });
        timer.start();
        Probe::run(store, [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(store.info(number).workerCovered, quint64(500));
        QVERIFY(affinity);
        std::printf("GUI ingress worst us %.3f; publication worst us %.3f; full drain elapsed us "
                    "%.3f (includes worker/wait)\n",
                    worst / 1000.0, Probe::worstPublicationNanos(store) / 1000.0,
                    timer.nsecsElapsed() / 1000.0);
    }
    void safeIntegerEveryAdmissionPath() {
        ConfigSchema schema(D::group({{"n", D::leaf(T::SafeInteger, qint64(9007199254740991LL))}}));
        QVERIFY(schema.valid());
        QVERIFY(!schema.validate({"n"}, qint64(9007199254740992LL)).ok());
        QVERIFY(!schema.validate({"n"}, 1.5).ok());
        for (auto kind :
             {ConfigLayerKind::File, ConfigLayerKind::Environment, ConfigLayerKind::Runtime}) {
            ConfigLayers engine(schema, {{"input",
                                          kind,
                                          ConfigAdmissionPolicy::FieldFallback,
                                          {{"source", {}, {{"n", qint64(9007199254740992LL)}}}},
                                          false}});
            QVERIFY(engine.valid());
            QCOMPARE(engine.value({"n"})->toLongLong(), 9007199254740991LL);
            QVERIFY(!engine.diagnostics("input", "source").isEmpty());
        }
        auto mapped = mapEnvironment(schema, {{"N", {"n"}}}, {{"N", "9007199254740992"}});
        QVERIFY(!mapped.ok());
        ConfigLayers user(schema, {{"user",
                                    ConfigLayerKind::File,
                                    ConfigAdmissionPolicy::FieldFallback,
                                    {{"source", {}, {}}},
                                    true}});
        QVERIFY(!user.setUser({"n"}, qint64(9007199254740992LL)).accepted);
    }
    void partialRoutesAndLifetime() {
        auto o = options();
        o.schema = ConfigSchema(D::group({{"a", D::group({{"n", D::leaf(T::Integer, 0)}})},
                                          {"b", D::group({{"n", D::leaf(T::Integer, 0)}})}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"a", {"a"}, {}}, {"b", {"b"}, {}}},
                     true}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        store.setUser({"a", "n"}, 1);
        store.setUser({"b", "n"}, 2);
        Probe::run(store, [](auto &w) {
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::None});
            w.scheduleBackend({false, ConfigMemoryBackend::Fault::BeforeOperation});
        });
        store.save();
        QTRY_COMPARE(complete.size(), 3);
        const auto result = qvariant_cast<ConfigResult>(complete[2][0]);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("partial"));
        QCOMPARE(result.publications.size(), 2);
        QCOMPARE(result.coveredRevisions.size(), 1);
        store.flush();
        store.flush();
        QTRY_COMPARE(complete.size(), 5);
        for (int i : {3, 4}) {
            const auto retained = qvariant_cast<ConfigResult>(complete[i][0]);
            QCOMPARE(retained.publicationState, QString("partial"));
            QCOMPARE(retained.publications.size(), 2);
        }
        auto *temporary = new ConfigStore(options());
        QTRY_COMPARE(temporary->status(), ConfigStore::Status::Ready);
        bool callback = false;
        auto *context = new QObject;
        temporary->save(context, [&](const auto &) { callback = true; });
        delete context;
        QSignalSpy done(temporary, &ConfigStore::completed);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!callback);
        Probe::run(*temporary,
                   [](auto &w) { w.scheduleBackend({true, ConfigMemoryBackend::Fault::None}); });
        temporary->setUser(number, 2);
        temporary->save();
        // The consumer no longer owns cancellation. Release the deterministic
        // backend after deletion through an independent context, then prove settlement.
        QPointer<ConfigWorker> detachedWorker;
        bool captured = false;
        Probe::run(
            *temporary, [&](auto &w) { detachedWorker = &w; }, this, [&] { captured = true; });
        QTRY_VERIFY(captured);
        QElapsedTimer timer;
        timer.start();
        delete temporary;
        QVERIFY(timer.elapsed() < 100);
        QVERIFY(detachedWorker);
        QMetaObject::invokeMethod(
            detachedWorker,
            [detachedWorker] {
                if (detachedWorker)
                    detachedWorker->releaseBackend();
            },
            Qt::QueuedConnection);
        QTRY_VERIFY(!detachedWorker);
    }
    void equalIntentAndSaveBoundaries() {
        ConfigStore store(options());
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy complete(&store, &ConfigStore::completed);
        store.setUser(number, 0);
        store.save();
        store.setUser(number, 1);
        store.save();
        store.setUser(number, 0);
        store.save();
        store.flush();
        QTRY_COMPARE(complete.size(), 7);
        QSet<QByteArray> revisions;
        for (int i : {1, 3, 5}) {
            auto result = qvariant_cast<ConfigResult>(complete[i][0]);
            QCOMPARE(result.status, QString("saved"));
            QCOMPARE(result.inputPrefix, quint64((i + 1) / 2));
            revisions.insert(result.coveredRevisions.value("memory").token);
        }
        QCOMPARE(revisions.size(), 3);
        QCOMPARE(qvariant_cast<ConfigResult>(complete[6][0]).status, QString("nothing_to_save"));
        bool read = false;
        QVariantMap raw;
        Probe::run(
            store,
            [&](auto &w) {
                auto id =
                    w.backend()->submit({ConfigBackendRequest::Operation::Read, "memory", {}, {}});
                raw = decodeMemoryValue(w.backend()->take(id)->bytes);
            },
            this, [&] { read = true; });
        QTRY_VERIFY(read);
        QVERIFY(valueAt(raw, number));
        QCOMPARE(valueAt(raw, number)->toLongLong(), 0);
    }
    void terminalPendingSettlement_data() {
        QTest::addColumn<bool>("masked");
        QTest::addColumn<int>("count");
        for (bool masked : {false, true})
            for (int count : {1, 2})
                QTest::newRow(qPrintable(QString("masked-%1-count-%2").arg(masked).arg(count)))
                    << masked << count;
    }
    void terminalPendingSettlement() {
        QFETCH(bool, masked);
        QFETCH(int, count);
        ConfigStore store(options(masked));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser(widgets, QVariantList{6});
        QTRY_COMPARE(store.info(widgets).workerCovered, quint64(1));
        const auto authoritative = store.value(widgets).toList();
        QSignalSpy complete(&store, &ConfigStore::completed);
        Probe::run(store, [](auto &w) { w.holdInputs(true); });
        QList<QVariantList> seen;
        store.setUser(widgets, store.value(widgets).toList() + QVariantList{4}, this,
                      [&](const auto &r) {
                          QCOMPARE(r.status, QString("failed"));
                          seen.append(store.value(widgets).toList());
                      });
        if (count == 2)
            store.setUser(widgets, store.value(widgets).toList() + QVariantList{5}, this,
                          [&](const auto &r) {
                              QCOMPARE(r.status, QString("failed"));
                              seen.append(store.value(widgets).toList());
                          });
        const auto local = store.value(widgets).toList();
        store.save();
        store.close();
        Probe::run(store, [](auto &w) {
            w.fail("terminal-before-input");
            w.releaseInputs(1);
        });
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(seen.first(), count == 1 ? authoritative : local);
        Probe::run(store, [](auto &w) { w.holdInputs(false); });
        QTRY_COMPARE(complete.size(), count + 2);
        QCOMPARE(seen.last(), authoritative);
        QCOMPARE(store.value(widgets).toList(), authoritative);
        QCOMPARE(store.inputs().size(), count + 1);
        QCOMPARE(store.status(), ConfigStore::Status::Closed);
        for (const auto &args : complete)
            QCOMPARE(qvariant_cast<ConfigResult>(args[0]).status, QString("failed"));
    }
    void readOnlyIngressHasNoProjection() {
        ConfigStore store(options(false, false));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy rejected(&store, &ConfigStore::inputRejected);
        bool callback = false;
        auto handle = store.setUser(number, 7, this, [&](const auto &r) {
            QCOMPARE(r.status, QString("failed"));
            QCOMPARE(store.value(number).toLongLong(), 0);
            callback = true;
        });
        QVERIFY(!handle.accepted);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(store.value(number).toLongLong(), 0);
        QVERIFY(store.inputs().isEmpty());
        QVERIFY(!callback);
        QTRY_VERIFY(callback);
    }
};
QTEST_GUILESS_MAIN(WorkerTest)
#include "test_worker.moc"
