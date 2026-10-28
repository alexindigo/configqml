// SPDX-License-Identifier: LGPL-3.0-or-later
#include "storeprobe.h"
#include <QTest>
using namespace configqml;
static ConfigStoreOptions options() {
    ConfigStoreOptions o;
    o.schema = ConfigSchema(ConfigDescriptor::group(
        {{"a", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)},
         {"b", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"memory", {}, {}}},
                 true}};
    return o;
}
class ConsumerLifetimeTest : public QObject {
    Q_OBJECT
private slots:
    void destroyAtBoundary_data() {
        QTest::addColumn<QString>("boundary");
        for (const auto &name : {"ingress-rejection", "ingress-change", "group-publication",
                                 "closing-status", "completion-signal", "completion-callback"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void destroyAtBoundary() {
        QFETCH(QString, boundary);
        QPointer<ConfigStore> store = new ConfigStore(options());
        QTRY_COMPARE(store->status(), ConfigStore::Status::Ready);
        bool callback = false, second = false;
        if (boundary == "ingress-rejection") {
            connect(store, &ConfigStore::inputRejected, this, [&] { delete store.data(); });
            auto handle = store->setUser({"a"}, QString("bad"));
            QVERIFY(!handle.accepted);
        } else if (boundary == "ingress-change") {
            connect(store, &ConfigStore::valuesChanged, this, [&] { delete store.data(); });
            auto handle = store->setUser({"a"}, 1);
            QVERIFY(handle.accepted);
        } else if (boundary == "group-publication") {
            store->setUser({"a"}, 1);
            store->setUser({"b"}, 2);
            QTRY_COMPARE(store->info({"b"}).workerCovered, quint64(2));
            connect(store, &ConfigStore::valuesChanged, this, [&] { delete store.data(); });
            store->reset({}, "user", this, [&](const auto &) { callback = true; });
        } else if (boundary == "closing-status") {
            connect(store, &ConfigStore::statusChanged, this, [&] { delete store.data(); });
            store->close(this, [&](const auto &) { callback = true; });
        } else if (boundary == "completion-signal") {
            connect(store, &ConfigStore::completed, this, [&] { delete store.data(); });
            store->save(this, [&](const auto &) { callback = true; });
        } else {
            store->save(this, [&](const auto &) {
                callback = true;
                delete store.data();
            });
            store->save(this, [&](const auto &) { second = true; });
        }
        QTRY_VERIFY(store.isNull());
        QCOMPARE(callback, boundary == "completion-callback");
        QVERIFY(!second);
    }
};
QTEST_GUILESS_MAIN(ConsumerLifetimeTest)
#include "test_consumer_lifetime.moc"
