// SPDX-License-Identifier: LGPL-3.0-or-later
// Independent gate's exact schedule: two changed leaves, grouped reset, first
// native facade notification synchronously destroys the facade.
#include "configfacade.h"
#include <QTest>
using namespace configqml;
class GateLifetimeTest : public QObject {
    Q_OBJECT
private slots:
    void groupedFacadeDestruction() {
        ConfigStoreOptions o;
        o.schema = ConfigSchema(ConfigDescriptor::group(
            {{"a", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)},
             {"b", ConfigDescriptor::leaf(ConfigDescriptor::Type::SafeInteger, 0)}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"memory", {}, {}}},
                     true}};
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"a"}, 1);
        store.setUser({"b"}, 2);
        QTRY_COMPARE(store.info({"b"}).workerCovered, quint64(2));
        QPointer<ConfigFacade> facade = new ConfigFacade(&store);
        int notifications = 0;
        connect(facade, &ConfigFacade::settingChanged, this, [&] {
            ++notifications;
            delete facade.data();
        });
        bool completed = false;
        store.reset({}, "user", this, [&](const auto &r) {
            QCOMPARE(r.status, QString("accepted"));
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(facade.isNull());
        QCOMPARE(notifications, 1);
        QCOMPARE(store.value({"a"}).toLongLong(), 0);
        QCOMPARE(store.value({"b"}).toLongLong(), 0);
    }
};
QTEST_GUILESS_MAIN(GateLifetimeTest)
#include "test_gate_lifetime.moc"
